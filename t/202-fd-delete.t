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

my $fname= $dp->temp_path . '/202-testfile.txt';
unlink( $fname );
ok( ! -f $fname, 'test file unlinked' );

$dp->send('fd.open', 'temp.f', 'write,create', $fname);
$dp->recv_ok( qr/^fd.state	temp.f	file	write.*$fname/m, 'create file command' );

$dp->send('fd.pipe', 'temp.r', 'temp.w');
$dp->recv_ok( qr/^fd.state	temp.r	pipe	from	temp.w/m, 'pipe read end' );
$dp->recv_ok( qr/^fd.state	temp.w	pipe	to	temp.r/m, 'pipe write end' );
$dp->discard_response;

$dp->send('fd.delete', 'null');
$dp->recv_ok( qr/^error\t/m, 'can\'t delete null' );

$dp->send('fd.delete', 'control.cmd');
$dp->recv_ok( qr/^error\t/m, 'can\'t delete control.cmd' );

$dp->send('fd.delete', 'temp.f');
$dp->recv_ok( qr/^fd.state	temp.f	deleted/m, 'deleted file handle' );

$dp->send('fd.delete', 'temp.r');
$dp->recv_ok( qr/^fd.state	temp.r	deleted/m, 'deleted pipe read-end' );

$dp->send('statedump');
$dp->recv_ok( qr/^fd.state	temp.w	pipe	to	\?$/m, 'write-end reference is removed' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;
unlink( $fname );
