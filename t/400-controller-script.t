#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i', '-vv');

my $script= 'print STDERR ">>>> STARTING\n";'
	.'$|=1; print "echo\ttest\n";'
	.'while (<STDIN>) { print STDERR ">>>> $_"; exit 42 if $_ =~ /^test$/m; }'
	.'exit(1);';
$dp->send("service.args	controller	perl	-e	$script");
$dp->send("service.fds	controller	control.event	control.cmd	stderr");
$dp->discard_response;

$dp->send("service.start	controller");
$dp->recv_ok( qr/^service.state	controller.*down.*exit	42/m, 'controller echo test' );

$dp->send("terminate	0");
$dp->exit_is( 0 );

done_testing;