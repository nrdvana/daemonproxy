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

$dp->send('service.args	foo	perl	-e	print "signal.clear\tSIGINT\t1\n";');
$dp->send('service.fds	foo	control.event	control.cmd	stderr');
$dp->send('service.autostart	foo	1	SIGINT');
$dp->recv_ok( qr/^service.autostart	foo	1	SIGINT/m, 'trigger set' );

kill INT => $dp->pid;
$dp->recv_ok( qr/^service.state	foo	up/m, 'service started from sigint' );
$dp->recv_ok( qr/^service.state	foo	down.*exit	0/m, 'service exited cleanly' );
$dp->send('statedump');
$dp->send('echo	done');
$dp->recv_ok( qr/(.*)^done$/ms );
ok( ! ($dp->last_captures->[0] =~ /^signal	SIGINT/m), 'no signal pending' );

kill INT => $dp->pid;
$dp->recv_ok( qr/^service.state	foo	up/m, 'service started from sigint' );
$dp->recv_ok( qr/^service.state	foo	down.*exit	0/m, 'service exited cleanly' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;