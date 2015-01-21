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
$dp->timeout(0.5);

$dp->send('service.args', 'foo', 'perl', '-e', 'setpgrp;$|=1;print "ready\n";sleep 1000;');
$dp->send('service.delete', 'foo');
$dp->recv_ok( qr/^service.state\tfoo\tdeleted/m, 'deleted' );

$dp->send('statedump');
$dp->send('echo', 'end');
$dp->recv_ok( qr/(.*)\nend$/ms, 'collect statedump' );

is( index($dp->last_captures->[0], 'foo'), -1, 'foo not mentioned in statedump' );

$dp->send('service.args', 'foo', 'sleep', '5');
$dp->recv_ok( qr/^service.args\tfoo\tsleep/m, 'service name re-usable' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;