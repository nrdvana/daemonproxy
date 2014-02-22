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
$dp->recv_ok( qr/^foo/m, 'test comm' );

kill INT => $dp->pid;
$dp->recv_ok( qr/^signal	SIGINT/m );

$dp->send("echo\tbar");
$dp->recv_ok( qr/^bar/m, 'test comm' );

kill SIGUSR1 => $dp->pid;
$dp->recv_ok( qr/^signal	SIGUSR1/m );

kill SIGUSR2 => $dp->pid;
$dp->recv_ok( qr/^signal	SIGUSR2/m );

$dp->send("terminate");
$dp->exit_is( 6 );

done_testing;