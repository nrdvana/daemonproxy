#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->timeout(2);
$dp->run('-i');
my @names;
my $error;
# Fill the available range with ordinary commands, then exercise each failure path.
for my $i (1..4096) {
    my $name= "limit$i";
    $dp->send('fd.open', $name, 'read', '/dev/null');
    die 'fd.open response timed out' unless $dp->recv_stdout(qr/^(fd.state\t\Q$name\E\tfile|error\t[^\n]*)/m);
    if ($dp->last_captures->[0] =~ /^error/) {
        $error= $dp->last_captures->[0];
        last;
    }
    push @names, $name;
}
like($error || '', qr/^error\topen failed:/, 'exhaustion follows the open failure path');
ok(@names > 0, 'some descriptors were allocated');
$dp->send('fd.pipe', 'full.r', 'full.w');
$dp->recv_stdout_ok(qr/^error\t.*pipe/m, 'out-of-range pipe allocation fails');
$dp->send('echo', 'still-responsive');
$dp->recv_stdout_ok(qr/^still-responsive$/m, 'supervisor remains responsive at the limit');

my $freed= pop @names;
$dp->send('fd.delete', $freed);
$dp->recv_stdout_ok(qr/^fd.state\t\Q$freed\E\tdeleted/m);
# One free slot is insufficient for a pipe.  The low end must also be released.
$dp->send('fd.pipe', 'partial.r', 'partial.w');
$dp->recv_stdout_ok(qr/^error\t.*pipe/m, 'pipe fails when only one in-range slot remains');
$dp->send('fd.open', 'recovered', 'read', '/dev/null');
$dp->recv_stdout_ok(qr/^fd.state\trecovered\tfile/m, 'failed pipe released its in-range end');
$dp->send('terminate', 0);
$dp->exit_is(0);
done_testing;
