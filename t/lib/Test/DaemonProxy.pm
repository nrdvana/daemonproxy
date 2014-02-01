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
	return bless { timeout => 0.5, buffer => '' }, $class;
}

sub dp_pid      { $_[0]{dp_pid} }
sub send_handle { $_[0]{send_handle} }
sub recv_handle { $_[0]{recv_handle} }
sub timeout     { @_ > 1? ($_[0]{timeout}= $_[1]) : $_[0]{timeout}; }

sub run {
	my ($self, @argv)= @_;
	die "Run was already called on this object"
		if defined $self->{dp_pid};
	
	my ($dp_to_script_w, $dp_to_script_r, $script_to_dp_w, $script_to_dp_r);
	pipe($dp_to_script_r, $dp_to_script_w) or die "pipe: $!";
	pipe($script_to_dp_r, $script_to_dp_w) or die "pipe: $!";
	
	# Launch DaemonProxy instance
	defined ($self->{dp_pid}= fork()) or die "fork: $!";
	if ($self->dp_pid == 0) {
		open STDIN, ">&", $script_to_dp_r or die "dup pipe to stdin: $!";
		open STDOUT, ">&", $dp_to_script_w or die "dup pipe to stdout: $!";
		exec($self->binary_path, @argv)
			or warn "exec(daemonproxy): $!";
		# make a sharp exit, without running cleanup code that could interfere with the parent process
		exec('/bin/false') || 	POSIX::_exit(2);
	}
	
	$self->{send_handle}= $script_to_dp_w;
	$self->{recv_handle}= $dp_to_script_r;
	$self->{recv_handle}->blocking(0);
	$self->{send_handle}->autoflush(1);
	1;
}

sub send {
	my ($self, $msg)= @_;
	print "send: \"$msg\"\n";
	$self->send_handle->print($msg."\n");
}

sub _read_more {
	my ($self)= @_;
	if (IO::Select->new( $self->recv_handle )->can_read($self->timeout)) {
		my $got= sysread($self->recv_handle, $self->{buffer}, 1024, length($self->{buffer}));
		if (!$got) {
			close($self->recv_handle);
			delete $self->{recv_handle};
			return 0;
		}
		return 1;
	}
	return 0;
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

sub response_like {
	my ($self, $pattern, $description)= @_;
	$pattern= qr/(?:$pattern)/pm # make pattern be multi-line and preserve position info
		unless "$pattern" =~ /^\(\?\^?[a-ln-oq-z]*p[a-ln-oq-z]*m/;
	$description ||= "response contains $pattern";
	while (1) {
		if ($self->{buffer} =~ $pattern) {
			Test::More::pass($description);
			# remove all of buffer up til matched line
			my $next_lf= index(${^POSTMATCH}, "\n");
			$self->{buffer}= $next_lf >= 0? substr(${^POSTMATCH}, $next_lf+1) : ${^POSTMATCH};
			return 1;
		}
		if (!$self->_read_more) {
			Test::More::fail($description);
			Test::More::diag("buffer contains: ".Dumper($self->{buffer}));
			return 0;
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

