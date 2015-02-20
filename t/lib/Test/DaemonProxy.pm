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
	$SIG{INT}= $SIG{TERM}= $SIG{QUIT}= $SIG{HUP}= sub { print STDERR "# test script killed\n"; exit(2) };
}

sub binary_path {
	my $self= shift;
	$self->{binary_path} ||= do {
		my $path= ($ENV{builddir} || 'build').'/daemonproxy';
		-f $path or Test::More::BAIL_OUT("Cannot stat \"$path\".  Set env 'builddir' to correct directory");
		$path;
	};
}

sub temp_path {
	my $self= shift;
	$self->{temp_path} ||= do {
		my $path= ($ENV{tempdir} || 'build');
		-d $path or Test::More::BAIL_OUT("Cannot write tempdir \"$path\".  Set env 'tempdir' to correct directory");
		$path;
	};
}

sub new {
	my $class= shift;
	return bless { timeout => 0.5 }, $class;
}

sub dp_pid        { $_[0]{dp_pid} }
*pid= *dp_pid;
sub dp_fds        { $_[0]{dp_fds} }
sub dp_stdin      { $_[0]{dp_fds}[0]{handle} }
sub dp_stdout     { $_[0]{dp_fds}[1]{handle} }
sub dp_stderr     { $_[0]{dp_fds}[2]{handle} }
sub last_captures { $_[0]{last_captures} }
sub timeout       { @_ > 1? ($_[0]{timeout}= $_[1]) : $_[0]{timeout}; }

sub run {
	my ($self, @argv)= @_;
	die "Run was already called on this object"
		if defined $self->{dp_pid};
	my %opts= %{ pop @argv }
		if @argv && (ref $argv[-1]||'') eq 'HASH';
	my @parent_fds;
	my @child_fds;
	for (my $i= 0; $i < 3 || defined $opts{"fd_$i"}; $i++) {
		if ($opts{"fd_$i"}) {
			my ($f1, $f2)= (ref $opts{"fd_$i"} eq 'ARRAY')? @{ $opts{"fd_$i"} } : ( $opts{"fd_$i"}, undef );
			push @child_fds, $f1;
			push @parent_fds, { handle => $f2, buffer => '' };
		}
		else {
			pipe(my ($r,  $w)) or die "pipe: $!";
			push @parent_fds, { handle => $i? $r : $w, buffer => '' };
			push @child_fds,  $i? $w : $r;
		}
	}
	
	# Launch DaemonProxy instance
	defined ($self->{dp_pid}= fork()) or die "fork: $!";
	if ($self->dp_pid == 0) {
		for (my $i= 0; $i < @child_fds; $i++) {
			POSIX::dup2(fileno($child_fds[$i]), $i)
				or die "dup pipe to $i: $!";
		}
		POSIX::close($_) for (scalar @child_fds .. 1023);
		my @strace= !$opts{strace}? ()
			: $opts{strace} eq '1'? ('strace')
			: ref $opts{strace} eq 'ARRAY'? ('strace', @{$opts{strace}})
			: ('strace', '-e', 'trace='.$opts{strace});
		exec(@strace, $self->binary_path, @argv)
			or warn "exec(daemonproxy): $!";
		# make a sharp exit, without running cleanup code that could interfere with the parent process
		POSIX::_exit(2);
	}
	
	for (@parent_fds) {
		next unless defined $_->{handle};
		$_->{handle}->autoflush(1);
		$_->{handle}->blocking(0) unless $_ eq $parent_fds[0];
	}
	$self->{dp_fds}= \@parent_fds;
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
	my $self= shift;
	croak "argument has embedded newline or tab" if grep { /[\t\n]/ } @_;
	my $msg= join("\t", @_);
	Test::More::note("send: "._quote_str($msg));
	local $SIG{PIPE}= sub {};
	$self->dp_stdin->print($msg."\n");
}

sub terminate_ok {
	my $self= shift;
	$self->send('terminate', 0);
	$self->exit_is( 0 );
}

sub _read_more {
	my ($self, $inputs)= @_;
	my %ready= map { $_ => 1 }
		IO::Select->new( grep { defined } map { $_->{handle} } @$inputs )
			->can_read($self->timeout);
	my $result= 0;
	for (@$inputs) {
		next unless defined $_->{handle} and $ready{$_->{handle}};
		$result= 1;
		my $got= sysread($_->{handle}, $_->{buffer}, 1024, length($_->{buffer}));
		if (!$got) {
			close($_->{handle});
			$_->{handle}= undef;
			next;
		}
		Test::More::note("recv: "._quote_str($_))
			for (substr($_->{buffer}, -$got) =~ /^.*$/mg);
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
		for my $input (@$inputs) {
			for (my $p= 0; $p < @$patterns; $p++) {
				# Check each pattern against the buffer
				my @captures;
				if (@captures= ($input->{buffer} =~ $patterns->[$p])) {
					my $match= ${^MATCH};
					my $line_end= -1;
					# Make sure we have the whole line it matched on
					if (length($match) && rindex($match, "\n") == length($match)-1) {
						$line_end= length(${^PREMATCH}) + length($match);
					} elsif (index(${^POSTMATCH}, "\n") >= 0) {
						$line_end= length(${^PREMATCH}) + length($match) + index(${^POSTMATCH}, "\n") + 1;
					} elsif (!defined $input->{handle}) {
						$line_end= length($input->{buffer});
					} else {
						next;
					}
					# Now remove all buffer up to and including the line the match occurred on.
					my $removed= $line_end > 0? substr($input->{buffer}, 0, $line_end) : '';
					substr($input->{buffer}, 0, $line_end)= '';
					$self->{last_match}= $match;
					$self->{last_pattern_idx}= $p;
					$self->{last_input_removed}= $removed;
					$self->{last_captures}= \@captures;
					return 1;
				}
			}
		}
		$self->_read_more([ grep { $_->{output} } @{$self->{dp_fds}} ])
			or return 0;
	}
}

sub discard_response {
	my $self= shift;
	while ($self->_read_more([ @{$self->{dp_fds}}[1,2] ])) {}
	$self->{dp_fds}[1]{buffer}= '';
	$self->{dp_fds}[2]{buffer}= '';
}

sub discard_stdout {
	my $self= shift;
	while ($self->_read_more([ @{$self->{dp_fds}}[1] ])) {}
	$self->{dp_fds}[1]{buffer}= '';
}

sub discard_stderr {
	my $self= shift;
	while ($self->_read_more([ @{$self->{dp_fds}}[2] ])) {}
	$self->{dp_fds}[2]{buffer}= '';
}

sub recv {
	my ($self, @patterns)= @_;
	$self->_recv_pattern([ @{$self->{dp_fds}}[1,2] ], \@patterns);
}

sub recv_stdout {
	my ($self, @patterns)= @_;
	$self->_recv_pattern([ @{$self->{dp_fds}}[1] ], \@patterns);
}

sub recv_stderr {
	my ($self, @patterns)= @_;
	$self->_recv_pattern([ @{$self->{dp_fds}}[2] ], \@patterns);
}

sub recv_ok {
	my $self= shift;
	my $pattern= shift;
	$_[0] ||= "response contains $pattern";
	if ($self->recv($pattern)) {
		goto &Test::More::pass;
	} else {
		Test::More::diag("stdout buffer: ".Data::Dumper->new([ $self->{dp_fds}[1]{buffer} ])->Terse(1)->Dump);
		Test::More::diag("stderr buffer: ".Data::Dumper->new([ $self->{dp_fds}[2]{buffer} ])->Terse(1)->Dump);
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
		Test::More::diag("stdout buffer: ".Data::Dumper->new([ $self->{dp_fds}[1]{buffer} ])->Terse(1)->Dump);
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
		Test::More::diag("stderr buffer: ".Data::Dumper->new([ $self->{dp_fds}[2]{buffer} ])->Terse(1)->Dump);
		goto &Test::More::fail;
	}
}

my $next_sync= 0;
sub sync {
	my $self= shift;
	my $id= 'sync_'.++$next_sync.'_';
	$self->send('echo', $id);
	$self->recv(qr/$id/) or croak "sync didn't complete";
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
	for (@{ $self->{dp_fds} || [] }) {
		close $_->{handle}
			if defined $_->{handle};
	}
}

sub DESTROY { $_[0]->cleanup(); }

1;
