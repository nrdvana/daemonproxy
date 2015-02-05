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
	$dp->recv_ok( qr|^fd.state\tfd1\tsocket\tunix,stream,bind,listen=\d+\t\Q$tempdir\E/test2.sock$|m, 'socket bound and listening' );

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
	
	$dp->recv_ok( qr/^service.state\ttest_unix.*exit\t0/m, 'test script accepted connection' );
	
	$dp->send('fd.socket', 'fd1', 'unix,mkdir', "$tempdir/foo/bar/baz");
	$dp->recv_ok( qr|^fd.state\tfd1\tsocket\tunix,stream,bind,mkdir\t\Q$tempdir\E/foo/bar/baz$|m, 'socket bound to path' );
	ok( -S "$tempdir/foo/bar/baz", 'parent directories created' );
	
};

$dp->send('terminate', 0);
$dp->exit_is( 0 );
done_testing;