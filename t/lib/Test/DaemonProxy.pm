package Test::DaemonProxy;
use strict;
use warnings;
require Test::More;
require IO::Select;
use IO::Handle;
use POSIX ':sys_wait_h';
use Data::Dumper 'Dumper';
use Time::HiRes 'sleep';
use Carp;

BEGIN {
	# make sure cleanup code runs
	$SIG{INT}= $SIG{TERM}= $SIG{QUIT}= $SIG{HUP}= sub { print STDERR "# killed\n"; exit(2) };
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
sub last_captures { $_[0]{last_captures} }
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

my %_escape_mapping= (
    "\0" => '\0',
    "\n" => '\n',
    "\r" => '\r',
    "\t" => '\t',
    "\f" => '\f',
    "\b" => '\b',
    "\a" => '\a',
    "\e" => '\e',
);
sub _escape_char {
    exists $_escape_mapping{$_[0]}?
        $_escape_mapping{$_[0]}
        : sprintf((ord $_[0] <= 0xFF)? "\\x%02X" : "\\x{%X}", ord $_[0]);
}
sub _quote_str {
	my $s= shift;
	$s =~ s/([\0-\x1F\x7F-\xFF])/ _escape_char($1) /eg;
	qq("$s");
}

sub send {
	my ($self, $msg)= @_;
	Test::More::note("send: "._quote_str($msg));
	local $SIG{PIPE}= sub {};
	$self->dp_stdin->print($msg."\n");
}

sub _read_more {
	my ($self, $inputs)= @_;
	my %ready= map { $_ => 1 }
		IO::Select->new( grep { defined } map { ${$_->[0]} } @$inputs )
			->can_read($self->timeout);
	my $result= 0;
	for (@$inputs) {
		my ($fd_ref, $buf_ref)= @$_;
		next unless defined $$fd_ref and $ready{$$fd_ref};
		my $got= sysread($$fd_ref, $$buf_ref, 1024, length($$buf_ref));
		if (!$got) {
			close($$fd_ref);
			$$fd_ref= undef;
			next;
		}
		Test::More::note("recv: "._quote_str($_))
			for (substr($$buf_ref, -$got) =~ /^.*$/mg);
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
		sleep(0.1);
	}
	return undef;
}

sub _recv_pattern {
	my ($self, $inputs, $patterns)= @_;
	for my $pattern (@$patterns) {
		$pattern= qr/(?:$pattern)/pm # make pattern be multi-line and preserve position info
			unless "$pattern" =~ /^\(\?\^?[a-ln-oq-z]*p[a-ln-oq-z]*m/;
	}
	while (1) {
		for (@$inputs) {
			my ($io_ref, $buf_ref)= @$_;
			for (my $p= 0; $p < @$patterns; $p++) {
				# Check each pattern against the buffer
				my @captures;
				if (@captures= ($$buf_ref =~ $patterns->[$p])) {
					my $match= ${^MATCH};
					my $line_end= -1;
					# Make sure we have the whole line it matched on
					if (length($match) && rindex($match, "\n") == length($match)-1) {
						$line_end= length(${^PREMATCH}) + length($match);
					} elsif (index(${^POSTMATCH}, "\n") >= 0) {
						$line_end= length(${^PREMATCH}) + length($match) + index(${^POSTMATCH}, "\n") + 1;
					} elsif (!defined $$io_ref) {
						$line_end= length($$buf_ref);
					} else {
						next;
					}
					# Now remove all buffer up to and including the line the match occurred on.
					my $removed= $line_end > 0? substr($$buf_ref, 0, $line_end) : '';
					substr($$buf_ref, 0, $line_end)= '';
					$self->{last_match}= $match;
					$self->{last_pattern_idx}= $p;
					$self->{last_input_removed}= $removed;
					$self->{last_captures}= \@captures;
					return 1;
				}
			}
		}
		$self->_read_more($inputs)
			or return 0;
	}
}

sub discard_response {
	my $self= shift;
	while ($self->_read_more([
		[ \$self->{dp_stdout}, \$self->{dp_stdout_buf} ],
		[ \$self->{dp_stderr}, \$self->{dp_stderr_buf} ]
		])) {}
	$self->{dp_stdout_buf}= '';
	$self->{dp_stderr_buf}= '';
}

sub discard_stdout {
	my $self= shift;
	while ($self->_read_more([
		[ \$self->{dp_stdout}, \$self->{dp_stdout_buf} ],
		])) {}
	$self->{dp_stdout_buf}= '';
}

sub discard_stderr {
	my $self= shift;
	while ($self->_read_more([
		[ \$self->{dp_stderr}, \$self->{dp_stderr_buf} ]
		])) {}
	$self->{dp_stderr_buf}= '';
}

sub recv {
	my ($self, @patterns)= @_;
	my $inputs= [
		[ \$self->{dp_stdout}, \$self->{dp_stdout_buf} ],
		[ \$self->{dp_stderr}, \$self->{dp_stderr_buf} ]
		];
	$self->_recv_pattern($inputs, \@patterns);
}

sub recv_stdout {
	my ($self, @patterns)= @_;
	my $inputs= [
		[ \$self->{dp_stdout}, \$self->{dp_stdout_buf} ],
		];
	$self->_recv_pattern($inputs, \@patterns);
}

sub recv_stderr {
	my ($self, @patterns)= @_;
	my $inputs= [
		[ \$self->{dp_stderr}, \$self->{dp_stderr_buf} ]
		];
	$self->_recv_pattern($inputs, \@patterns);
}

sub recv_ok {
	my $self= shift;
	my $pattern= shift;
	$_[0] ||= "response contains $pattern";
	if ($self->recv($pattern)) {
		goto &Test::More::pass;
	} else {
		Test::More::diag("stdout buffer: ".Data::Dumper->new([ $self->{dp_stdout_buf} ])->Terse(1)->Dump);
		Test::More::diag("stderr buffer: ".Data::Dumper->new([ $self->{dp_stderr_buf} ])->Terse(1)->Dump);
		goto &Test::More::fail;
	}
}
*response_like= *recv_ok;

sub recv_stdout_ok {
	my $self= shift;
	my $pattern= shift;
	$_[0] ||= "response contains $pattern";
	if ($self->recv_stdout($pattern)) {
		goto &Test::More::pass;
	} else {
		Test::More::diag("stdout buffer: ".Data::Dumper->new([ $self->{dp_stdout_buf} ])->Terse(1)->Dump);
		goto &Test::More::fail;
	}
}

sub recv_stderr_ok {
	my $self= shift;
	my $pattern= shift;
	$_[0] ||= "response contains $pattern";
	if ($self->recv_stderr($pattern)) {
		goto &Test::More::pass;
	} else {
		Test::More::diag("stderr buffer: ".Data::Dumper->new([ $self->{dp_stderr_buf} ])->Terse(1)->Dump);
		goto &Test::More::fail;
	}
}


sub exit_is {
	my $self= shift;
	my $exitcode= shift;
	$_[0] ||= "exit with code $exitcode";
	my $wstat= $self->_collect_exit_status;
	if (!defined $wstat) {
		Test::More::note("wait() timed out");
		goto &Test::More::fail;
	}
	elsif ($wstat & 127) {
		Test::More::note("died on signal ".($? & 127));
		goto &Test::More::fail;
	}
	else {
		unshift @_, $wstat>>8, $exitcode;
		goto &Test::More::is;
	}
}

sub cleanup {
	my $self= shift;
	
	$self->dp_pid and kill SIGQUIT => $self->dp_pid;
	delete $self->{dp_pid};
	for (qw: dp_stdin dp_stdout dp_stderr :) {
		close delete $self->{$_}
			if defined $self->{$_};
	}
}

sub DESTROY { $_[0]->cleanup(); }

