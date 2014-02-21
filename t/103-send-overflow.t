#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('--stdin');

$dp->send("blah\t" x 1000);
$dp->recv_ok( qr/error:.*long/, 'long line causes error' );

$dp->discard_response;
$dp->send("# blah\t" x 1000);
$dp->send("echo\t-marker-");
$dp->recv_ok( qr/^-marker-$/, 'long comment doens\'t generate an error' );

done_testing;