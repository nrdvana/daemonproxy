#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('-i');

$dp->send("blah\t" x 1000);
$dp->recv_ok( qr/error\t.*long/, 'long line causes error' );

$dp->discard_response;
$dp->send("# blah\t" x 1000);
$dp->send('echo', '-marker-');
$dp->recv_ok( qr/^-marker-$/, 'long comment doesn\'t generate an error' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );
done_testing;