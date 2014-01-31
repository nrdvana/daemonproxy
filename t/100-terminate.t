#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('--stdin');

$dp->send("terminate");
$dp->exit_is(0);

done_testing;