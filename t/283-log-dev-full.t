#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $unique_string= "qwerty-".time.rand;
my ($test_id)= ($FindBin::Script =~ /^(\d+)/)
	or die "Can't determine test_id";

sub log_to_dev_full {
	my $blocked_write_count= shift;

	my $dp= Test::DaemonProxy->new;
	$dp->timeout(.5);

	my $full= IO::File->new("/dev/full", 'w');
	my $tracefile= $dp->temp_path . "/$test_id-strace.txt";
	my $log_redir_file= $dp->temp_path . "/$test_id-log.txt";
	unlink $tracefile;
	unlink $log_redir_file;
	$dp->run('-i', { fd_2 => $full, strace => [ '-D', '-o', $tracefile ]});

	# write 5 invalid commands
	for (1..$blocked_write_count) {
		$dp->send('invalid');
		$dp->recv(qr/invalid/);
	}
	
	sleep 1; # Wait longer than daemonproxy's LOG_WRITE_TIMEOUT

	# Change logging to a different file
	$dp->send('fd.open', 'log.w', 'write,create', $log_redir_file);
	$dp->recv_ok( qr/^ fd.state \t log\.w .* \t \Q$log_redir_file\E /mx, 'new logfile created' );
	$dp->send('log.dest', 'fd', 'log.w' );

	$dp->send('log.filter', 'trace');
	$dp->send('echo', $unique_string);
	$dp->recv_ok( qr/^\Q$unique_string\E$/m, 'have all output' );
	$dp->terminate_ok;
	
	my $trace_out= do { open my $f, '<', $tracefile or die; local $/= undef; <$f> };
	my $log_out=   do { open my $f, '<', $log_redir_file or die; local $/= undef; <$f> };
	return { trace_out => $trace_out, log_out => $log_out };
}

subtest blocked_no_overflow => sub {
	my $out= log_to_dev_full(5); # shouldn't overflow log buffer

	# ensure that daemonproxy didn't busy-loop while calling write on file descriptor 2
	my $count= @{[ $out->{trace_out} =~ /write\(2/g ]};
	cmp_ok( $count, '<', 10, 'small number of attempts to write to fd 2' );
	cmp_ok( $count, '>', 3,  'daemonproxy did retry the write' );

	# ensure that we got all 5 commands which were queued in the log output buffer
	$count= @{[ $out->{log_out} =~ /unknown command/ig ]};
	is( $count, 5, '5 invalid command messages' );

	# and also the unique string
	like( $out->{log_out}, qr/\Q$unique_string\E/, 'unique string in output' );

	done_testing;
};

# Now, repeat the experiment and overflow the pipe while it is blocked
subtest blocked_and_overflow => sub {
	my $out= log_to_dev_full(500); # should overflow log buffer

	# ensure that daemonproxy didn't busy-loop while calling write on file descriptor 2
	my $count= @{[ $out->{trace_out} =~ /write\(2/g ]};
	cmp_ok( $count, '<', 10, 'small number of attempts to write to fd 2' );
	cmp_ok( $count, '>', 3,  'daemonproxy did retry the write' );

	# ensure that we got an overflow messages once logging was resumed
	like( $out->{log_out}, qr/lost \d+ log messages/, 'got lost-messages message' );

	# and also the unique string
	like( $out->{log_out}, qr/\Q$unique_string\E/, 'unique string in output' );

	done_testing;
};

done_testing;