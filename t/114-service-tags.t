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

# Set args initially
$dp->send('service.tags', 'foo', 'mytag');
$dp->response_like( qr!^service.tags\tfoo\tmytag$!, 'set tags' );

# should be able to overwrite it
$dp->send('service.tags', 'foo', 'color=blue');
$dp->response_like( qr!^service.tags\tfoo\tcolor=blue$!, 'overwrite tags' );

# unset it
$dp->send('service.tags', 'foo');
$dp->response_like( qr!^service.tags\tfoo\t$!, 'unset tags' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;