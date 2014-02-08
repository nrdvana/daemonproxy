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

# Test return value of true
$dp->send("service.args.set	foo	true");
$dp->send("service.start	foo");
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\texit\t0\t!m, 'service exited 0' );

# Test return value of script
$dp->send("service.args.set	foo	perl	-e	exit(42)");
$dp->send("service.start	foo");
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\texit\t42\t!m, 'service exited 42' );

# Test script exit on signal
$dp->send("service.args.set	foo	perl	-e	kill KILL=>\$\$");
$dp->send("service.start	foo");
$dp->response_like( qr!^service.state\tfoo\tup!m, 'service started' );
$dp->response_like( qr!^service.state\tfoo\tdown\t.*\tsignal.*=SIGKILL\t!m, 'service signalled SIGKILL' );

$dp->send("terminate");
$dp->exit_is( 0 );

done_testing;