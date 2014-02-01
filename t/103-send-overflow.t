#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('--stdin');

$dp->send("blah\t" x 1000);
$dp->response_like( qr/error:.*long/, 'long line causes error' );

$dp->send("# blah\t" x 1000);
ok( ! $dp->_read_more, 'comment ignored even though too long' );

$dp->send("echo\t-marker-");
$dp->response_like( qr/^-marker-/, 'can still send/receive' );

done_testing;