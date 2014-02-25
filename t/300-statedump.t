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

$dp->send("service.fds	foo	null	null	null");
$dp->send("service.args	bar	a	b	c");
$dp->recv( qr/^service.args	/m );
kill HUP => $dp->dp_pid;

$dp->recv( qr/^service.args/m );

$dp->send("statedump");
$dp->send("echo	complete");
$dp->recv_ok( qr/^fd.state	null	/m, 'null fd' );
$dp->recv_ok( qr/^service.state	bar/m, 'service bar' );
$dp->recv_ok( qr/^service.state	foo/m, 'service foo' );
$dp->recv_ok( qr/^signal	SIGHUP/m, 'signal HUP' );
$dp->recv_ok( qr/^complete/m, 'statedump complete' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;