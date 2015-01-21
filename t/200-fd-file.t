#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp= Test::DaemonProxy->new;
$dp->run('-i', '-v');

my $fname= $dp->temp_path . '/200-testfile.txt';

unlink( $fname );
ok( ! -f $fname, 'test file unlinked' );

$dp->send('fd.open', 'temp.w', 'write,create', $fname);
$dp->recv_ok( qr/^fd.state\ttemp.w\tfile\t.*write.*\t$fname$/m, 'create file command' );

ok( -f $fname, 'file created' );

$dp->send('fd.open', 'temp.r', 'read', $fname);
$dp->recv_ok( qr/^fd.state	temp.r	file	.*read.*	$fname$/m, 'open created file' );

my $script= '$|=1; print "test $$\n"; my $x=<STDIN>; exit ($x =~ /test $$/? 0 : 1)';
$dp->send('service.args',  'test', 'perl', '-e', $script );
$dp->send('service.fds',   'test', 'temp.r', 'temp.w', 'stderr');
$dp->send('service.start', 'test');
$dp->recv_ok( qr/^service.state	test	.*exit	0/m, 'test script able to write and read' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;
unlink( $fname );
