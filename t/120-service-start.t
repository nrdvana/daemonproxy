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

# Test return values
for my $ret (0, 42, 255) {
	$dp->send("service.args	foo	perl	-e	exit($ret)");
	$dp->send("service.start	foo");
	$dp->recv_ok( qr!^service.state\tfoo\tup!m, 'service started' );
	$dp->recv_ok( qr!^service.state\tfoo\tdown\t.*\texit\t$ret\t!m, "service exited $ret" );
}

# Test script exit on signal
for my $sig (qw: SIGTERM SIGHUP SIGINT SIGKILL :) {
	$dp->send("service.args	foo	perl	-e	kill $sig=>\$\$");
	$dp->send("service.start	foo");
	$dp->recv_ok( qr!^service.state\tfoo\tup!m, 'service started' );
	$dp->recv_ok( qr!^service.state\tfoo\tdown\t[^\t]+\t[^\t]+\tsignal\t$sig\t!m, "service signalled $sig" );
}

# Test uptime and timestamps
$dp->timeout(4);
$dp->send('service.args	foo	perl	-e	use Time::HiRes qw:clock_gettime CLOCK_MONOTONIC sleep:; $|=1; print clock_gettime(CLOCK_MONOTONIC).qq{\n};sleep 2;print clock_gettime(CLOCK_MONOTONIC).qq{\n};');
$dp->send("service.fds	foo	null	stderr	stderr");
$dp->send("service.start	foo");
$dp->recv_ok( qr!^service.state\tfoo\tup\t(\d+)!m, 'service up' );
my $t_start= clock_gettime(CLOCK_MONOTONIC);
my $ts_up= $dp->last_captures->[0];
$dp->recv_ok( qr!^(\d+)!m, 'service prints startime' );
my $ts_begin= $dp->last_captures->[0];
cmp_ok( $ts_up, '<=', $t_start, 'up ts valid' )
	or diag "t_start: $ts_begin, ts_up: $ts_up";

$dp->recv_ok( qr!^(\d+)!m, 'service prints endtime' );
my $ts_end= $dp->last_captures->[0];
$dp->recv_ok( qr!^service.state\tfoo\tdown\t(.*)$!m, 'service down' );
my $t_finish= clock_gettime(CLOCK_MONOTONIC);
my ($ts_down, $pid, $exreason, $exval, $uptime, $downtime)= split /\t/, $dp->last_captures->[0];
cmp_ok( $ts_end, '<=', $ts_down, 'down ts valid' )
	or diag "t_end: $ts_end, ts_down: $ts_down";
cmp_ok( abs($uptime - ($t_finish - $t_start)), '<', 1, 'uptime near expected value' )
	or diag "uptime: $uptime, t_end: $t_finish, t_start: $t_start";

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;