#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('--exec-on-exit', 't/util/exit-42.sh', '--version');
$dp->exit_is( 42, '--version exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exec-on-exit', 't/util/exit-42.sh', '--help');
$dp->exit_is( 42, '--help exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exec-on-exit', 't/util/exit-42.sh', '--nonsense-argument-that-fails');
$dp->exit_is( 42, 'invalid cmdline exec()s' );

$dp= Test::DaemonProxy->new;
$dp->run('--exec-on-exit', 't/util/exit-42.sh', '-c', 'config-file-that-doesnt-exist');
$dp->exit_is( 42, 'open cfgfile failure exec()s' );

for my $sig ( qw( SIGABRT SIGSEGV ) ) {
	$dp= Test::DaemonProxy->new;
	$dp->run('--exec-on-exit', 't/util/exit-42.sh');
	sleep 0.1;
	kill $sig => $dp->dp_pid;
	$dp->exit_is( 42, "$sig exec()s" );
}

done_testing;