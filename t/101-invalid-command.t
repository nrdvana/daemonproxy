#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;

$dp->run('-i');

$dp->send("typo");
$dp->response_like(qr/^error\t.*typo/m);

$dp->send('echo', '-marker-');
$dp->response_like( qr/^-marker-$/m, 'can still send/receive' );

$dp->send('echo', '-marker-');
$dp->response_like( qr/^-marker-$/m, 'can still send/receive' );

$dp->terminate_ok;

done_testing;