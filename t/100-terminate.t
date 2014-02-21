#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate");
$dp->discard_response;
$dp->exit_is(6, 'exit with EXIT_TERMINATE');

$dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate	3");
$dp->discard_response;
$dp->exit_is(3);

$dp= Test::DaemonProxy->new;
$dp->run('--stdin');
$dp->send("terminate	255");
$dp->discard_response;
$dp->exit_is(255);

done_testing;