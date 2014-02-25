#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';
my $dp;

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--fd-pool', '20x1K');
$dp->send("terminate	0");
$dp->exit_is( 0 );

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--fd-pool', '5x200');
$dp->send("terminate	0");
$dp->exit_is( 0 );

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--fd-pool', '5x50M');
$dp->recv_ok( qr/^warn.*maximum/m, '50MB object gets truncated' );
$dp->send("terminate	0");
$dp->exit_is( 0 );

$dp= Test::DaemonProxy->new;
$dp->run('-i', '--fd-pool', '1');
$dp->recv_ok( qr/^warn.*minimum/m, 'increased to minimum file handle count' );

# count handles
$dp->send("statedump");
$dp->send("echo	done");
$dp->recv_ok( qr/(.*)^done$/ms, 'captured statedump' );

my @handles= $dp->last_captures->[0] =~ /fd.state	(\w+)/g;
cmp_ok( scalar @handles, '>', 1, 'more than one handle' );

# shoud not be able to create any FDs
$dp->send("fd.open	foo	read	/dev/null");
$dp->recv_ok( qr/^error\t.*allocate.*file/m, 'file pool full' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;