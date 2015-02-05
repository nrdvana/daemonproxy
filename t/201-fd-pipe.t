#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;
use Time::HiRes 'sleep';

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');

subtest plain_pipe => sub {
	$dp->send('fd.pipe', 'temp.r', 'temp.w');
	$dp->recv_ok( qr/^fd.state	temp.r	pipe	from	temp.w/m, 'pipe read end' );
	$dp->recv_ok( qr/^fd.state	temp.w	pipe	to	temp.r/m, 'pipe write end' );

	my $script= '$|=1; use strict; use warnings; print "test $$\n"; my $x=<STDIN>; exit ($x =~ /test $$/? 0 : 1)';
	$dp->send('service.args',  'test', 'perl', '-e', $script );
	$dp->send('service.fds',   'test', 'temp.r', 'temp.w', 'stderr');
	$dp->send('service.start', 'test');
	$dp->recv_ok( qr/^service.state	test	.*exit	0/m, 'test script able to write and read' );
};

subtest unix_sockets => sub {
	$dp->send('fd.pipe', 'temp.1', 'temp.2', 'unix');
	$dp->recv_ok( qr/^fd.state	temp.1	pipe	unix,stream,to	temp.2/m, 'socketpair 1' );
	$dp->recv_ok( qr/^fd.state	temp.2	pipe	unix,stream,to	temp.1/m, 'socketpair 2' );
	
	my $script= '$|=1;
	  open(my $fd1, "<&=3") or die "fdopen 3: $!";
	  open(my $fd2, "<&=4") or die "fdopen 4: $!";
	  send($fd1,"test $$\n",0) or die "send1: $!";
	  defined recv($fd2, my $x, 999, 0) or die "recv1: $!";
	  send($fd2,"test $$\n",0) or die "send2: $!";
	  defined recv($fd1, my $x2, 999, 0) or die "recv2: $!";
	  exit (($x eq $x2 && $x2 eq "test $$\n")? 0 : 1)';
	$script =~ s/[\n\t]//g;
	$dp->send('service.args',  'test_unix', 'perl', '-e', $script );
	$dp->send('service.fds',   'test_unix', 'null', 'stderr', 'stderr', 'temp.1', 'temp.2');
	$dp->send('service.start', 'test_unix');
	$dp->recv_ok( qr/^service.state	test_unix	.*exit	0/m, 'test script able to use bidirectional pipe' );
};

subtest unix_datagram => sub {
	$dp->send('fd.pipe', 'temp.1', 'temp.2', 'unix,dgram');
	$dp->recv_ok( qr/^fd.state	temp.1	pipe	unix,dgram,to	temp.2/m, 'socketpair 1' );
	$dp->recv_ok( qr/^fd.state	temp.2	pipe	unix,dgram,to	temp.1/m, 'socketpair 2' );
	
	my $script= '$|=1;
	  open(my $fd1, "<&=3") or die "fdopen 3: $!";
	  open(my $fd2, "<&=4") or die "fdopen 4: $!";
	  send($fd1,"test $$\n",0) or die "send1: $!";
	  defined recv($fd2, my $x, 999, 0) or die "recv1: $!";
	  send($fd2,"test $$\n",0) or die "send2: $!";
	  defined recv($fd1, my $x2, 999, 0) or die "recv2: $!";
	  exit (($x eq $x2 && $x2 eq "test $$\n")? 0 : 1)';
	$script =~ s/[\n\t]//g;
	$dp->send('service.args',  'test_dgram', 'perl', '-e', $script );
	$dp->send('service.fds',   'test_dgram', 'null', 'stderr', 'stderr', 'temp.1', 'temp.2');
	$dp->send('service.start', 'test_dgram');
	$dp->recv_ok( qr/^service.state	test_dgram	.*exit	0/m, 'test script able to use bidirectional pipe' );
};

$dp->terminate_ok;
done_testing;