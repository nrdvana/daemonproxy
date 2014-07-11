#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep', 'clock_gettime', 'CLOCK_MONOTONIC';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');

$dp->timeout(3);
$dp->send('service.args', 'foo', 'perl', '-e', 'use Time::HiRes qw:clock_gettime CLOCK_MONOTONIC:; $|=1; print clock_gettime(CLOCK_MONOTONIC).qq{\n};');
$dp->send('service.fds', 'foo', 'null', 'stderr', 'stderr');

# Test scheduling start for future time
my $now= clock_gettime(CLOCK_MONOTONIC);
$dp->send('service.start', 'foo', int($now + 5));

$dp->recv_ok( qr!^service.state\tfoo\tstart\t(\d+)\t-\t-\t-\t-\t-$!m, 'service start pending' );
my $start_ts= $dp->last_captures->[0];
cmp_ok( $now, '<', $start_ts, 'starttime is in future (this test has a race condition)' );

$dp->timeout(6);
$dp->recv_ok( qr!^(\d+)!m, 'service prints startime' );
my $actual_start_ts= $dp->last_captures->[0];
cmp_ok( $start_ts, '<=', $actual_start_ts, 'started near correct time' );

$dp->timeout(3);
$dp->recv_ok( qr!^service.state\tfoo\tdown!m, 'service ended' );

# Test ability to cancel a scheduled start
$now= clock_gettime(CLOCK_MONOTONIC);
$dp->send('service.start', 'foo', int($now + 5));
$dp->recv_ok( qr!^service.state\tfoo\tstart\t(\d+)\t-\t-\t-\t-\t-$!m, 'service start pending' );
$dp->send('service.start', 'foo', '-');
$dp->recv_ok( qr!^service.state\tfoo\t(\w+)!m, 'got service state change' );
is( $dp->last_captures->[0], 'down', 'service transition start->down with no "up"' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;