#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--help');
$dp->recv_ok( qr/options/i, 'usage message' );
$dp->recv_ok( qr/--interactive/, 'various options' );
$dp->recv_ok( qr/--exit-exec/, 'various options' );
$dp->exit_is( 1 );

done_testing;