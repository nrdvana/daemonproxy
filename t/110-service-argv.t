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
$dp->send("service.args.set	foo	/bin/sh	-c	echo testing");
$dp->response_like( qr!^service.args\tfoo\t/bin/sh\t-c\techo testing$!, 'set args' );

# should be able to overwrite it
$dp->send("service.args.set	foo	/bin/sleep	1");
$dp->response_like( qr!^service.args\tfoo\t/bin/sleep\t1$!, 'overwrite args' );

# read it again
$dp->send("service.args	foo");
$dp->response_like( qr!^service.args\tfoo\t/bin/sleep\t1$!, 'read args' );

$dp->send("terminate");
$dp->exit_is( 0 );

done_testing;