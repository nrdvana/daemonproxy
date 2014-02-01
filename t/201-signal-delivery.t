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

$dp->send("echo\tfoo");
$dp->response_like( qr/foo/, 'test comm' );

kill INT => $dp->pid;
$dp->response_like( qr/SIGINT/ );

$dp->send("echo\tbar");
$dp->response_like( qr/bar/, 'test comm' );

kill SIGUSR1 => $dp->pid;
$dp->response_like( qr/SIGUSR1/ );

kill SIGUSR2 => $dp->pid;
$dp->response_like( qr/SIGUSR2/ );

$dp->send("terminate");
$dp->exit_is( 0 );

done_testing;