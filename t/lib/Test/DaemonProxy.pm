package Test::DaemonProxy;
use strict;
use warnings;
require Test::More;
require IO::Select;
use IO::Handle;
use POSIX ':sys_wait_h';
use Data::Dumper 'Dumper';
use Carp;

BEGIN {
	# make sure cleanup code runs
	$SIG{INT}= $SIG{TERM}= $SIG{QUIT}= sub { print STDERR "# killed\n"; exit(2) };
}

sub binary_path {
	my $self= shift;
	$self->{binary_path} ||= do {
		my $path= ($ENV{builddir} || 'build').'/daemonproxy';
		-f $path or Test::More::BAIL_OUT("Cannot stat \"$path\".  Set env 'builddir' to correct directory");
		$path;
	};
}

sub new {
	my $class= shift;
	return bless { timeout => 0.5, dp_stdout_buf => '', dp_stderr_buf => '' }, $class;
}

sub dp_pid      { $_[0]{dp_pid} }
*pid= *dp_pid;
sub dp_stdin    { $_[0]{dp_stdin} }
sub dp_stdout   { $_[0]{dp_stdout} }
sub dp_stderr   { $_[0]{dp_stderr} }
sub timeout     { @_ > 1? ($_[0]{timeout}= $_[1]) : $_[0]{timeout}; }

sub run {
	my ($self, @argv)= @_;
	die "Run was already called on this object"
		if defined $self->{dp_pid};
	
	pipe(my ($dp_stdin_r,  $dp_stdin_w))  or die "pipe: $!";
	pipe(my ($dp_stdout_r, $dp_stdout_w)) or die "pipe: $!";
	pipe(my ($dp_stderr_r, $dp_stderr_w)) or die "pipe: $!";
	
	# Launch DaemonProxy instance
	defined ($self->{dp_pid}= fork()) or die "fork: $!";
	if ($self->dp_pid == 0) {
		open STDIN,  ">&", $dp_stdin_r  or die "dup pipe to stdin: $!";
		open STDOUT, ">&", $dp_stdout_w or die "dup pipe to stdout: $!";
		open STDERR, ">&", $dp_stderr_w or die "dup pipe to stderr: $!";
		exec($self->binary_path, @argv)
			or warn "exec(daemonproxy): $!";
		# make a sharp exit, without running cleanup code that could interfere with the parent process
		exec('/bin/false') || 	POSIX::_exit(2);
	}
	
	$self->{dp_stdin}= $dp_stdin_w;
	$self->{dp_stdin}->autoflush(1);
	
	$self->{dp_stdout}= $dp_stdout_r;
	$self->{dp_stdout}->blocking(0);
	
	$self->{dp_stderr}= $dp_stderr_r;
	$self->{dp_stderr}->blocking(0);
	1;
}

sub send {
	my ($self, $msg)= @_;
	print "# send: \"$msg\"\n";
	$self->dp_stdin->print($msg."\n");
}

sub _read_more {
	my ($self)= @_;
	my $result= 0;
	for (IO::Select->new( grep { defined } @{$self}{'dp_stdout','dp_stderr'} )->can_read($self->timeout)) {
		my $is_stdout= (defined $self->{dp_stdout} && $_ eq $self->{dp_stdout});
		my $buf= $is_stdout? \$self->{dp_stdout_buf} : \$self->{dp_stderr_buf};
		my $got= sysread($_, $$buf, 1024, length($$buf));
		if (!$got) {
			close(delete $self->{$is_stdout? 'dp_stdout' : 'dp_stderr'});
		}
		print map { chomp; "# recv: $_\n" } (substr($$buf, -$got) =~ /^.*$/mg);
		$result= 1;
	}
	return $result;
}

sub _collect_exit_status {
	my ($self)= @_;
	return $self->{dp_wstat} if defined $self->{dp_wstat};
	my $deadline= time + $self->timeout;
	while (1) {
		if (waitpid($self->dp_pid, WNOHANG) == $self->dp_pid) {
			delete $self->{dp_pid};
			return $self->{dp_wstat}= $?;
		}
		last if time > $deadline;
		select(undef, undef, undef, $deadline - time);
	}
	return undef;
}

sub flush_response {
	my $self= shift;
	while ($self->_read_more) {}
	$self->{dp_stdout_buf}= '';
	$self->{dp_stderr_buf}= '';
}

sub response_like {
	my $self= shift;
	my $pattern= shift;
	$pattern= qr/(?:$pattern)/pm # make pattern be multi-line and preserve position info
		unless "$pattern" =~ /^\(\?\^?[a-ln-oq-z]*p[a-ln-oq-z]*m/;
	$_[0] ||= "response contains $pattern";
	while (1) {
		if ($self->{dp_stdout_buf} =~ $pattern) {
			# remove all of buffer up til matched line
			my $next_lf= index(${^POSTMATCH}, "\n");
			$self->{dp_stdout_buf}= $next_lf >= 0? substr(${^POSTMATCH}, $next_lf+1) : ${^POSTMATCH};
			goto &Test::More::pass;
		}
		if (!$self->_read_more) {
			Test::More::diag("buffer contains: ".Dumper($self->{dp_stdout_buf}));
			goto &Test::More::fail;
		}
	}
}

sub stderr_like {
	my $self= shift;
	my $pattern= shift;
	$pattern= qr/(?:$pattern)/pm # make pattern be multi-line and preserve position info
		unless "$pattern" =~ /^\(\?\^?[a-ln-oq-z]*p[a-ln-oq-z]*m/;
	$_[0] ||= "response contains $pattern";
	while (1) {
		if ($self->{dp_stderr_buf} =~ $pattern) {
			# remove all of buffer up til matched line
			my $next_lf= index(${^POSTMATCH}, "\n");
			$self->{dp_stderr_buf}= $next_lf >= 0? substr(${^POSTMATCH}, $next_lf+1) : ${^POSTMATCH};
			goto &Test::More::pass;
		}
		if (!$self->_read_more) {
			Test::More::diag("buffer contains: ".Dumper($self->{dp_stderr_buf}));
			goto &Test::More::fail;
		}
	}
}

sub exit_is {
	my ($self, $exitcode, $description)= @_;
	$description ||= "exited with code $exitcode";
	my $wstat= $self->_collect_exit_status;
	if (!defined $wstat) {
		Test::More::fail($description);
		Test::More::diag("wait() timed out");
		return 0;
	}
	elsif ($wstat & 127) {
		Test::More::fail($description);
		Test::More::diag("died on signal ".($? & 127));
		return 0;
	}
	else {
		return Test::More::is($wstat>>8, $exitcode, $description);
	}
}

sub cleanup {
	my $self= shift;
	
	$self->dp_pid and kill SIGQUIT => $self->dp_pid;
	delete $self->{dp_pid};
	close delete $self->{send_handle}
		if defined $self->{send_handle};
	close delete $self->{recv_handle}
		if defined $self->{recv_handle};
}

sub DESTROY { $_[0]->cleanup(); }

