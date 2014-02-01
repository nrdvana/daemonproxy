#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--help');
$dp->response_like( qr/usage/i, 'usage message' );
$dp->response_like( qr/--stdin/, 'various options' );
$dp->response_like( qr/--exec-on-exit/, 'various options' );
$dp->exit_is(1);

done_testing;