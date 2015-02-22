#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use IO::Handle;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $unique_string= "qwerty-".time.rand;
my ($test_id)= ($FindBin::Script =~ /^(\d+)/)
	or die "Can't determine test_id";

my $dp= Test::DaemonProxy->new;
$dp->timeout(.5);

my $tracefile= $dp->temp_path . '/' . $test_id . '-strace.txt';
unlink $tracefile;

pipe(my $err_rd, my $err_wr) or die "pipe; $!";

# Fill up the pipe
$err_wr->blocking(0);
while ($err_wr->syswrite("x"x10)) {}
$err_wr->blocking(1);

# start daemonproxy on a full blocking pipe
$dp->run('-i', { fd_2 => [$err_wr, $err_rd], strace => [ '-D', '-o', $tracefile ]});

# write more than 4K of invalid commands (generating log messages, overflowing buffer)
for (1..40) {
	$dp->send('invalid', 'x'x100);
	$dp->recv_stdout_ok( qr/invalid/ );
}

# Now consume the pipe.
# Make sure we get a few 'invalid' log messages which are left over.
$dp->recv_stderr_ok( qr/lost \d+ log messages$/m );

# Make sure the log is flowing again.
$dp->send($unique_string);
$dp->recv_stderr_ok( qr/\Q$unique_string\E/ );

$dp->terminate_ok;

# Now, make sure daemonproxy only ever wrote to the pipe about 4 times:
#   - when it tried writing the first message, and found the pipe full
#   - when pipe became writeable and it flushed the whole buffer
#   - when it appended "lost X messages"
#   - when it wrote the unique_string message

my $trace_out= do { open my $f, '<', $tracefile or die; local $/= undef; <$f> };
my $count= @{[ $trace_out =~ /write\(2/g ]};
cmp_ok( $count, '<', 6, 'less than 6 write() calls to log pipe' );

done_testing;