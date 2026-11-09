#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('-i', '--service-pool', '2x2048');

$dp->send('echo', 'x' x 2048);
$dp->recv_ok(qr/^error\tline too long$/m, 'oversized command rejected');
$dp->send('echo', 'recovered');
$dp->recv_ok(qr/^recovered$/m, 'next command succeeds after overflow');

$dp->send('signal.clear', 'NOT_A_SIGNAL', 1);
$dp->recv_ok(qr/^error\tInvalid signal argument/m, 'unknown signal rejected');

# Identical names used to delete the same old object twice.
$dp->send('fd.open', 'same', 'read', '/dev/null');
$dp->recv_ok(qr/^fd.state\tsame\tfile/m, 'original handle created');
$dp->send('fd.pipe', 'same', 'same');
$dp->recv_ok(qr/^error\tfailed to create pipe/m, 'identical pipe names rejected');
$dp->send('statedump');
$dp->recv_ok(qr/^fd.state\tsame\tfile/m, 'original handle preserved');
$dp->send('echo', 'dumped');
$dp->recv_ok(qr/^dumped$/m);

# Enough metadata to reach the fields formerly overwritten inside the struct.
$dp->send('service.tags', 'pooled', 't' x 500);
$dp->recv_ok(qr/^service.tags\tpooled\tt{500}$/m, 'large pooled metadata stored');

# Exercise both launch arrays beyond the small sizes masked by stack alignment.
my @args= map { "arg$_" } 1..40;
$dp->send('service.args', 'pooled', 'perl', '-e',
    'exit(@ARGV == 40 && $ARGV[39] eq "arg40" ? 42 : 1)', @args);
$dp->recv_ok(qr/^service.args\tpooled/m, 'many arguments configured');
$dp->send('service.fds', 'pooled', ('null') x 40);
$dp->recv_ok(qr/^service.fds\tpooled/m, 'many descriptors configured');
$dp->send('service.start', 'pooled');
$dp->recv_ok(qr/^service.state\tpooled\tdown\t.*\texit\t42\t/m,
    'pooled service starts with all arguments and descriptors');
$dp->send('service.delete', 'pooled');
$dp->recv_ok(qr/^service.state\tpooled\tdeleted/m, 'exited service can be deleted');
$dp->send('terminate', 0);
$dp->exit_is(0);
done_testing;
