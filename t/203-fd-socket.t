#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;
use Time::HiRes 'sleep';
use Socket;

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');

my $tempdir= sprintf("%s/tmp/t%03d", $FindBin::Bin, do { $FindBin::Script =~ /(\d+)/? $1 : $$ });
system('mkdir','-p',$tempdir) == 0 or die;
system('rm','-r',$tempdir) == 0 or die;
system('mkdir','-p',$tempdir) == 0 or die;

subtest unix => sub {
	$dp->send('fd.socket', 'fd1', 'unix');
	$dp->recv_ok( qr/^fd.state\tfd1\tsocket\tunix,stream\t?$/m, 'socket created' );
	
	$dp->send('fd.socket', 'fd1', 'unix', "$tempdir/test.sock");
	$dp->recv_ok( qr/^fd.state\tfd1\tsocket\tunix,stream,bind\t\Q$tempdir\E\/test.sock$/m, 'socket bound to path' );

	$dp->send('fd.socket', 'fd1', 'unix,stream,listen', "$tempdir/test2.sock");
	$dp->recv_ok( qr|^fd.state\tfd1\tsocket\tunix,stream,bind,listen=\d+\t\Q$tempdir\E/test2.sock$|m, 'socket bound and listening' )
		or die;

	my $script= '
	$|=1;
	use strict; use warnings; use Socket;
	accept(my $sock, STDIN) or die "$!";
	recv($sock, my $buf, 999, 0);
	exit($buf eq "data"? 0 : 1);
	';
	$script =~ s/[\t\n]+/ /g;
	$dp->send('service.args',  'test_unix', 'perl', '-e', $script );
	$dp->send('service.fds',   'test_unix', 'fd1', 'stderr', 'stderr');
	$dp->send('service.start', 'test_unix');
	
	socket(my $sock, Socket::AF_UNIX(), Socket::SOCK_STREAM(), 0)
		or die "Can't create socket: $!";
	ok( connect($sock, Socket::sockaddr_un("$tempdir/test2.sock")), 'connect succeeded' )
		or diag("connect: $!");
	send($sock, "data", 0);
	
	$dp->recv_ok( qr/^service.state\ttest_unix.*exit\t0/m, 'test script accepted connection and received data' );
	
	$dp->send('fd.socket', 'fd1', 'unix,mkdir', "$tempdir/foo/bar/baz");
	$dp->recv_ok( qr|^fd.state\tfd1\tsocket\tunix,stream,bind,mkdir\t\Q$tempdir\E/foo/bar/baz$|m, 'socket bound to path' );
	ok( -S "$tempdir/foo/bar/baz", 'parent directories created' );
};

subtest tcp => sub {
	$dp->send('fd.socket', 'fd2', 'tcp');
	$dp->recv_ok( qr/^fd.state\tfd2\tsocket\tinet,stream\t?$/m, 'socket created' );
	
	$dp->send('fd.socket', 'fd2', 'tcp,listen=11', '*:11203');
	$dp->recv_ok( qr/^fd.state\tfd2\tsocket\tinet,stream,bind,listen=11\t*:11203$/m, 'socket bound to port 11203' )
		or die;
	
	my $script= '
	$|=1;
	use strict; use warnings; use Socket;
	accept(my $sock, STDIN) or die "accept: $!";
	recv($sock, my $buf, 999, 0);
	exit($buf eq "data"? 0 : 1);
	';
	$script =~ s/[\t\n]+/ /g;
	$dp->send('service.args',  'test_tcp', 'perl', '-e', $script);
	$dp->send('service.fds',   'test_tcp', 'fd2', 'stderr', 'stderr');
	$dp->send('service.start', 'test_tcp');
	
	socket(my $sock, Socket::AF_INET(), Socket::SOCK_STREAM(), 0)
		or diag "socket: $!";
	my $dest_addr= Socket::sockaddr_in(11203, Socket::inet_aton('127.0.0.1'));
	ok( connect($sock, $dest_addr), 'connect succeeded' )
		or diag("connect: $!");
	send($sock, "data", 0)
		or diag("send: $!");
	
	$dp->recv_ok( qr/^service.state\ttest_tcp.*exit\t0/m, 'test script accepted connection and received data' );
	
	$dp->send('fd.socket', 'fd2b', 'tcp', '*.11203');
	$dp->recv_ok( qr/^error\t.*bind/m, 'can\'t bind second socket to same port' );
};

subtest udp => sub {
	$dp->send('fd.socket', 'fd3', 'udp');
	$dp->recv_ok( qr/^fd.state\tfd3\tsocket\tinet,dgram\t?$/m, 'socket created' );
	
	$dp->send('fd.socket', 'fd3', 'udp', '*:10203');
	$dp->recv_ok( qr/^fd.state\tfd3\tsocket\tinet,dgram,bind\t*:10203$/m, 'socket bound to port 10203' )
		or die;
	
	my $script= '
	$|=1;
	use strict; use warnings; use Socket;
	recv($sock, my $buf, 999, 0);
	exit($buf eq "data"? 0 : 1);
	';
	$script =~ s/[\t\n+]/ /g;
	$dp->send('service.args',  'test_udp', 'perl', '-e', $script);
	$dp->send('service.fds',   'test_udp', 'fd3', 'stderr', 'stderr');
	$dp->send('service.start', 'test_udp');
	
	socket(my $sock, Socket::AF_INET(), Socket::SOCK_DGRAM(), 0)
		or diag "socket: $!";
	my $dest_addr= Socket::sockaddr_in(10203, Socket::inet_aton('127.0.0.1'));
	ok( send($sock, "data", 0, $dest_addr), 'sent datagram' )
		or diag("send: $!");
	
	$dp->recv_ok( qr/^service.state\ttest_udp.*exit\t0/m, 'test script received datagram' );
	
	$dp->send('fd.socket', 'fd3b', 'udp', '*:10203');
	$dp->recv_ok( qr/^error\t.*bind/m, 'can\'t bind second socket to same port' );
};

$dp->terminate_ok;

done_testing;
