#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('-i');

$dp->send("log.filter");
$dp->recv_ok( qr/^log.filter	debug$/m, 'default filter is debug' );

$dp->send('log.filter', '-');
$dp->recv_ok( qr/^log.filter	trace$/m, 'lower filter to trace' );

$dp->send('log.filter', '-');
$dp->recv_ok( qr/^log.filter	none$/m, 'lower bound reached' );

for (qw: trace debug info warning error fatal :) {
	$dp->send('log.filter', $_);
	$dp->recv_ok( qr/^log.filter	$_$/m, "by name: $_" );
}

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;