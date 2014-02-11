#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep', 'clock_gettime', 'CLOCK_MONOTONIC';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('--stdin');

# Test return values
for my $ret (0, 42, 255) {
	$dp->send("service.args.set	foo	perl	-e	exit($ret)");
	$dp->send("service.start	foo");
	$dp->recv_ok( qr!^service.state\tfoo\tup!m, 'service started' );
	$dp->recv_ok( qr!^service.state\tfoo\tdown\t.*\texit\t$ret\t!m, "service exited $ret" );
}

# Test script exit on signal
for my $sig (qw: SIGTERM SIGHUP SIGINT SIGKILL :) {
	$dp->send("service.args.set	foo	perl	-e	kill $sig=>\$\$");
	$dp->send("service.start	foo");
	$dp->recv_ok( qr!^service.state\tfoo\tup!m, 'service started' );
	$dp->recv_ok( qr!^service.state\tfoo\tdown\t.*\tsignal.*=${sig}\t!m, 'service signalled $sig' );
}

# Test uptime and timestamps
$dp->send("service.args.set	foo	sleep	2");
$dp->send("service.start	foo");
$ENV{DEBUG}= 1;
$dp->recv_ok( qr!^service.state\tfoo\tup\t(\d+)!m, 'service up' );
my ($ts_up, $t_start)= ($dp->last_captures->[0], clock_gettime(CLOCK_MONOTONIC));
cmp_ok( abs($t_start - $ts_up), '<', 1, 'up ts within one second' )
	or diag "t_start: $t_start, ts_up: $ts_up";

$dp->timeout(4);
$dp->recv_ok( qr!^service.state\tfoo\tdown\t(\d+).*uptime\t(\d+)!m, 'service down' );
my ($ts_down, $uptime, $t_end)= (@{$dp->last_captures}[0,1], clock_gettime(CLOCK_MONOTONIC));
cmp_ok( abs($t_end - $ts_down), '<', 1, 'down ts within one second' )
	or diag "t_end: $t_end, ts_down: $ts_down";
cmp_ok( abs($uptime - ($t_end - $t_start)), '<', 1, 'uptime near expected value' )
	or diag "uptime: $uptime, t_end: $t_end, t_start: $t_start";

$dp->send("terminate");
$dp->exit_is( 0 );

done_testing;