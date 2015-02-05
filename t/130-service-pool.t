#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;
use Time::HiRes 'sleep';
my $dp;

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--service-pool', '20x1K');
$dp->terminate_ok;

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--service-pool', '5x200');
$dp->terminate_ok;

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--service-pool', '6');
$dp->timeout(0.5);

# Allocate 6 services.
for (1..6) {
	$dp->send('service.args', "service$_", '/bin/true');
	$dp->recv_ok( qr/^service.args\tservice$_/m, "created service$_" );
}

# Allocate 7th should fail
$dp->send('service.args', 'service7', '/bin/true');
$dp->recv_ok( qr/^error\t.*allocate.*service/m, '7th fails' );

# free one
$dp->send('service.delete', 'service1');
$dp->recv_ok( qr/^service.state	service1	deleted/m, 'freed a slot' );

# new allocation should succeed
$dp->send('service.args', 'service7', '/bin/true');
$dp->recv_ok( qr/^service.args	service7/m, 'created service7' );

# free them all
for (2..7) {
	$dp->send('service.delete', "service$_");
	$dp->recv( qr/^service.state	service$_	deleted/m );
}

$dp->terminate_ok;

done_testing;