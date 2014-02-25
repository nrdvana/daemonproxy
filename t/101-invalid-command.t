#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('-i');

$dp->send("typo");
$dp->response_like(qr/^error\t.*typo/);

$dp->send("echo\t-marker-");
$dp->response_like( qr/^-marker-/, 'can still send/receive' );

$dp->send("echo\t-marker-");
$dp->response_like( qr/^-marker-/, 'can still send/receive' );

$dp->send("terminate	0");
$dp->exit_is(0);

done_testing;