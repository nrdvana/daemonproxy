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
$dp->send("service.args	foo	/bin/sh	-c	echo testing");
$dp->response_like( qr!^service.args\tfoo\t/bin/sh\t-c\techo testing$!, 'set args' );

# should be able to overwrite it
$dp->send("service.args	foo	/bin/sleep	1");
$dp->response_like( qr!^service.args\tfoo\t/bin/sleep\t1$!, 'overwrite args' );

# unset it
$dp->send("service.args	foo");
$dp->response_like( qr!^service.args\tfoo\t$!, 'unset args' );

$dp->send("terminate");
$dp->exit_is( 6 );

done_testing;