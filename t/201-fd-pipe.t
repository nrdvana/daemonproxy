#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('--stdin', '-v');

my $fname= $dp->temp_path . '200-testfile.txt';

unlink( $fname );
ok( ! -f $fname, 'test file unlinked' );

$dp->send("fd.pipe	temp.r	temp.w");
$dp->recv_ok( qr/^fd.state	temp.r	pipe_from	temp.w/m, 'pipe read end' );
$dp->recv_ok( qr/^fd.state	temp.w	pipe_to	temp.r/m, 'pipe write end' );

my $script= '$|=1; print "test $$\n"; my $x=<STDIN>; exit ($x =~ /test $$/? 0 : 1)';
$dp->send("service.args	test	perl	-e	$script" );
$dp->send("service.fds	test	temp.r	temp.w	stderr");
$dp->send("service.start	test");
$dp->recv_ok( qr/^service.state	test	.*exit	0/m, 'test script able to write and read' );

$dp->send("terminate");
$dp->exit_is( 6 );

done_testing;