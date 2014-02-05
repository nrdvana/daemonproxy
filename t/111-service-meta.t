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

# Set args initially
$dp->send("service.meta	foo");
$dp->response_like( qr/no such service/i, 'inspect nonexistent service' );

$dp->send("service.meta.set	foo	a=1");
$dp->response_like( qr/^service.meta	foo	a=1$/m, 'set meta' );

$dp->send("service.meta.set	foo	a=5	b=9");
$dp->response_like( qr/^service.meta	foo	a=5	b=9$/m, 'overwrite meta' );

$dp->send("service.meta.apply	foo	g=10");
$dp->response_like( qr/^service.meta	foo	a=5	b=9	g=10$/m, 'apply change with new value' );

$dp->send("service.meta.apply	foo	a=testing");
$dp->response_like( qr/^service.meta	foo	a=testing	b=9	g=10$/m, 'enlarge first value' );

$dp->send("service.meta.apply	foo	a=xyz");
$dp->response_like( qr/^service.meta	foo	a=xyz	b=9	g=10$/m, 'shrink first value' );

$dp->send("service.meta.apply	foo	a=123");
$dp->response_like( qr/^service.meta	foo	a=123	b=9	g=10$/m, 'replace first value' );

$dp->send("service.meta.apply	foo	b=2");
$dp->response_like( qr/^service.meta	foo	a=123	b=2	g=10$/m, 'replace second value' );

$dp->send("service.meta.apply	foo	b=qwertyuiop");
$dp->response_like( qr/^service.meta	foo	a=123	b=qwertyuiop	g=10$/m, 'enlarge second value' );

$dp->send("service.meta.apply	foo	b=");
$dp->response_like( qr/^service.meta	foo	a=123	b=	g=10$/m, 'shrink second value' );

$dp->send("service.meta.apply	foo	g=01");
$dp->response_like( qr/^service.meta	foo	a=123	b=	g=01$/m, 'replace last value' );

$dp->send("service.meta.apply	foo	g=203984");
$dp->response_like( qr/^service.meta	foo	a=123	b=	g=203984$/m, 'enlarge last value' );

$dp->send("service.meta.apply	foo	g=0");
$dp->response_like( qr/^service.meta	foo	a=123	b=	g=0$/m, 'shrink last value' );

$dp->send("terminate");
$dp->exit_is( 0 );

done_testing;