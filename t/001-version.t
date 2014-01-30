#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('--version');
$dp->response_like(qr/daemonproxy.*version/i, 'version message');
$dp->response_like(qr/build timestamp:/i, 'build info');
$dp->response_like(qr/git HEAD:/i, 'revision control info');
$dp->exit_is(1);

done_testing;