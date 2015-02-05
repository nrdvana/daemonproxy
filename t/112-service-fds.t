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
$dp->send('service.fds', 'foo', 'null', 'null', 'null');
$dp->response_like( qr!^service.fds\tfoo\tnull\tnull\tnull$!, 'set fds' );

# should be able to overwrite it
$dp->send('service.fds', 'foo', 'null', 'stdout', 'stdout');
$dp->response_like( qr!^service.fds\tfoo\tnull\tstdout\tstdout$!, 'overwrite fds' );

# read it again
$dp->send('service.fds', 'foo');
$dp->response_like( qr!^service.fds\tfoo\t$!, 'unset fds' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;