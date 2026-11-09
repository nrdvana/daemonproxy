#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes qw(clock_gettime CLOCK_MONOTONIC);

my $dp= Test::DaemonProxy->new;
$dp->run('-i');
for my $n ('9223372036854775808', '-9223372036854775809', '18446744073709551616') {
    $dp->send('terminate', $n);
    $dp->recv_ok(qr/^error\tExpected integer/m, "reject overflow $n");
}
for my $n ('9223372036854775807', '-9223372036854775808') {
    $dp->send('terminate.guard', '+', $n);
    $dp->send('terminate.guard', '-', $n);
    $dp->send('echo', "boundary-$n");
    $dp->recv_ok(qr/(.*?)^boundary-\Q$n\E$/ms, "64-bit boundary $n accepted");
    unlike($dp->last_captures->[0], qr/^error\t/m, 'guard boundary produced no errors');
}
for my $n ('-1', '2147483648', '9223372036854775807') {
    $dp->send('signal.clear', 'SIGUSR1', $n);
    $dp->recv_ok(qr/^error\tsignal count/m, "reject invalid signal count $n");
}
$dp->send('service.args', 'future', '/bin/true');
$dp->recv_ok(qr/^service.args\tfuture/m);
for my $n ('-1', '2147483648', '9223372036854775807') {
    $dp->send('service.start', 'future', $n);
    $dp->recv_ok(qr/^error\tinvalid timestamp/m, "reject invalid timestamp $n");
}
my $when= int(clock_gettime(CLOCK_MONOTONIC)) - 10;
$dp->send('service.start', 'future', $when);
$dp->recv_ok(qr/^service.state\tfuture\tup/m, 'timestamp ten seconds ago starts now');
$dp->send('terminate', 0);
$dp->exit_is(0);

for my $opts (
    ['--fd-pool', '18446744073709551616'],
    ['--fd-pool', '7x8589934592G'],
    ['--service-pool', '2x2147483648'],
    ['--exit-guard', '9223372036854775808'],
    ['--exit-guard', ''],
) {
    $dp= Test::DaemonProxy->new;
    $dp->run('-i', @$opts);
    $dp->exit_is(2, "invalid @$opts rejected");
}
done_testing;
