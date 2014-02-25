#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');
$dp->timeout(2);

$dp->send('service.args	foo	perl	-e	use Time::HiRes "sleep"; sleep 0.5;');
$dp->send('service.fds	foo	null	stderr	stderr');
$dp->send('service.triggers	foo	auto=1');
$dp->recv_ok( qr/^service.triggers	foo	auto/m, 'trigger set' );
$dp->recv_ok( qr/^service.state	foo	up/m, 'autostart trigger causes immediate up' );
$dp->recv_ok( qr/^service.state	foo	down.*exit	0/m, 'exited' );
$dp->recv_ok( qr/^service.state	foo	up/m, 'restart after exit' );

$dp->send('service.triggers	foo');
$dp->recv_ok( qr/^service.triggers	foo/m, 'no triggers' );
$dp->recv_ok( qr/^service.state	foo	down/m, 'down again' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;