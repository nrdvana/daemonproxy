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
mkdir("$FindBin::Bin/tmp");
mkdir($tempdir);

subtest unix => sub {
	$dp->send('fd.socket', 'fd1', 'unix');
	$dp->recv_ok( qr/^fd.state\tfd1\tunix,stream\t?$/m, 'socket created' );
	
	$dp->send('fd.socket', 'fd1', 'unix', "$tempdir/test.sock");
	$dp->recv_ok( qr/^fd.state\tfd1\tunix,stream\t\Q$tempdir\E\/test.sock$/m, 'socket bound to path' );

	$dp->send('fd.socket', 'fd1', 'unix,stream,listen', "$tempdir/test.sock");
	$dp->recv_ok( qr/^fd.state\tfd1\tunix,stream,bind,listen\t\Q$tempdir\E\/test.sock$/m, 'socket bound to path' );

	my $script= '
	$|=1;
	use strict; use warnings; use Socket;
	accept(my $sock, STDIN) or die "$!";
	exit(0);
	';
	$dp->send('service.args',  'test_unix', 'perl', '-e', $script );
	$dp->send('service.fds',   'test_unix', 'fd1', 'stderr', 'stderr');
	$dp->send('service.start', 'test_unix');
	
	socket(my $sock, Socket::AF_UNIX(), Socket::SOCK_STREAM(), 0)
		or die "Can't create socket: $!";
	ok( connect($sock, Socket::sockaddr_un("$tempdir/test.socket")) );
	
	$dp->recv_ok( qr/^service.state\ttest_unix.*exit\t0/m, 'test script accepted connection' );
};

$dp->send('terminate', 0);
$dp->exit_is( 0 );
done_testing;