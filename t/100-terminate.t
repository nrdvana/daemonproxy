#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate");
$dp->discard_response;
$dp->exit_is(6, 'exit with EXIT_TERMINATE');

$dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate	3");
$dp->discard_response;
$dp->exit_is(3);

$dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate	255");
$dp->discard_response;
$dp->exit_is(255);

$dp= Test::DaemonProxy->new;
$dp->run('--stdin', '--terminate-guard', 54321);
$dp->send("terminate	0");
$dp->recv_ok( qr/^error.*guard code/m, 'terminate fails without code' );
$dp->send("terminate	0	12345");
$dp->recv_ok( qr/^error.*incorrect/m, 'terminate fails with wrong code' );
$dp->send("terminate	0	54321");
$dp->recv_ok( qr/^error.*exec/m, 'terminate fails with correct code due to no exec args' );
$dp->send("terminate.guard	-	54321");
$dp->send("terminate	0");
$dp->exit_is( 0, 'exit 0 after disable terminate guard' );

done_testing;