#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--daemonize', '-c', '/dev/null');
$dp->recv_ok( qr/(\d+)/, 'got pid' )
	or die 'pid required to proceed';

my $pid= $dp->last_captures->[0];

$dp->exit_is( 0, 'parent exits' );

SKIP: {
	chomp(my $ps_info= `ps -p $pid -o sid= -o pgid= -o comm=`);
	is( $?, 0, 'ps command' )
		or skip 'ps output required for next tests', 3;

	my (undef, $sid, $pgid, $name)= split /\s+/, " $ps_info";
	is( $name, 'daemonproxy', 'process name' );
	is( $sid, $pid, 'session id' );
	is( $pgid, $pid, 'process group id' );
};

ok( kill(SIGKILL => $pid), 'send kill to daemon' );

done_testing;