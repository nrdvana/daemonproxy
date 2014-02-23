#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->timeout(0.5);

$dp->send('service.args	foo	perl	-e	setpgrp;$|=1;print "ready\n";sleep 1000;');
$dp->send('service.fds	foo	null	stderr	stderr');

$dp->send("service.start	foo");
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
ok( $dp->recv_stderr(qr!^ready$!m), 'service ready' );
$dp->send("service.signal	foo	SIGQUIT");
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\tsignal\tSIGQUIT\t!m, 'service signalled SIGQUIT' );

$dp->send("service.start	foo");
ok( $dp->recv_stderr(qr!^ready$!m), 'service ready' );
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->send("service.signal	foo	QUIT");
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\tsignal\tSIGQUIT\t!m, 'service signalled SIGQUIT' );

$dp->send("service.start	foo");
ok( $dp->recv_stderr(qr!^ready$!m), 'service ready' );
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->send("service.signal	foo	INT");
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\tsignal\tSIGINT\t!m, 'service signalled SIGINT' );

$dp->send("service.start	foo");
ok( $dp->recv_stderr(qr!^ready$!m), 'service ready' );
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->send("service.signal	foo	SIGQUIT	group");
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\tsignal\tSIGQUIT\t!m, 'process group signalled SIGHUP' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;