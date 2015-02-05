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
$dp->run('-i');

# Set args repeatedly, causing buffer to grow
for (my $i= 1; $i < 8; $i++) {
	my $str= '012345678	' x (10*$i);
	$dp->send("service.args	foo	$str");
	$dp->recv_ok( qr/^service.args	foo/m, "args of size ".length($str) );
}
# Then shrink it
$dp->send('service.args', 'foo', '');
$dp->recv_ok( qr/^service.args	foo\t?$/m, 'args of length 0' );

# verify that it checks service names before creating them
$dp->send('service.args', ("x"x 32), '/bin/true');
$dp->recv_ok( qr/^error	.*service.args\tx/m, 'service name length 32 fails' );

$dp->send('service.args', ("x"x 31), '/bin/true');
$dp->recv_ok( qr/^service.args\tx{31}\t/m, 'service name length 31 succeeds' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;