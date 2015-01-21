#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');

$dp->send('echo', 'foo');
$dp->recv_ok( qr/^foo/m, 'test comm' );

kill INT => $dp->pid;
$dp->recv_ok( qr/^signal	SIGINT	\d+	1/m, 'sigint reported' );

$dp->send('echo', 'bar');
$dp->recv_ok( qr/^bar/m, 'test comm' );

kill SIGUSR1 => $dp->pid;
$dp->recv_ok( qr/^signal	SIGUSR1	\d+	1/m, 'sigusr1 reported' );

kill SIGUSR2 => $dp->pid;
$dp->recv_ok( qr/^signal	SIGUSR2	\d+	1/m, 'sigusr2 reported' );

kill SIGUSR2 => $dp->pid;
$dp->recv_ok( qr/^signal	SIGUSR2	\d+	2/m, 'inc count of sigusr2' );

$dp->send('statedump');
$dp->send('echo', 'end');
$dp->recv_ok( qr/^signal	SIGUSR2	\d+	2/m, 'statedump shows signal' );
$dp->recv_ok( qr/^end$/m );

$dp->send('signal.clear', 'SIGUSR2', 1);
$dp->send('statedump');
$dp->send('echo', 'end');
$dp->recv_ok( qr/^signal	SIGUSR2	\d+	1/m, 'statedump shows half-cleared signal' );
$dp->recv_ok( qr/^end$/m );

$dp->send('signal.clear', 'SIGUSR2', 1);
$dp->send('statedump');
$dp->send('echo', 'end');
$dp->recv_ok( qr/(.*)\nend$/sm );
ok( index($dp->last_captures->[0], 'SIGUSR2') == -1, 'sigusr2 no longer in statedump' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;