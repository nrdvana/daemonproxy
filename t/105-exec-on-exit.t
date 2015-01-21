#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-v', '--exit-exec', '/bin/sh	-c	exit 42', '--version');
$dp->discard_response;
$dp->exit_is( 42, '--version exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exit-exec', '/bin/sh	-c	exit 42', '--help');
$dp->discard_response;
$dp->exit_is( 42, '--help exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exit-exec', '/bin/sh	-c	exit 42', '--nonsense-argument-that-fails');
$dp->discard_response;
$dp->exit_is( 42, 'invalid cmdline exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exit-exec', '/bin/sh	-c	exit 42', '-c', 'config-file-that-doesnt-exist');
$dp->discard_response;
$dp->exit_is( 42, 'open cfgfile failure exec()s' );

for my $sig ( qw( SIGABRT SIGSEGV ) ) {
	$dp= Test::DaemonProxy->new;
	$dp->run('--exit-exec', '/bin/sh	-c	exit 42');
	sleep 0.1;
	kill $sig => $dp->dp_pid;
	$dp->exit_is( 42, "$sig exec()s" );
}

$dp= Test::DaemonProxy->new;
$dp->run('-i');
$dp->send('terminate.exec_args', 'perl', '-e', 'exit 42');
$dp->send('echo', 'done');
$dp->recv( qr/^done/m );
kill SIGILL => $dp->pid;
$dp->exit_is( 42, "terminate.exec_args command" );

done_testing;