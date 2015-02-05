#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp;
$dp= Test::DaemonProxy->new;
$dp->run('-i');

my $sock_path= $dp->temp_path . '/450-socket';
my $sock_path2= $sock_path . '2';

unlink $sock_path, $sock_path2;
ok( ! -e $sock_path, "socket $sock_path does not exist" );
ok( ! -e $sock_path2, "socket $sock_path2 does not exist" );

$dp->send('socket.create', '-', $sock_path);
$dp->send('echo', 'done');
$dp->recv( qr/^done$/m );
ok( -S $sock_path, 'socket was created' );

$dp->send('socket.create', '', $sock_path2);
$dp->send('echo', 'done');
$dp->recv( qr/^done$/m );
ok( ! -e $sock_path, 'old socket deleted' );
ok( -S "$sock_path2", 'new socket created' );

use Socket;
socket(my $s, PF_UNIX, SOCK_STREAM, 0) || die "socket: $!";
connect($s, sockaddr_un($sock_path2)) || die "connect: $!";
$s->autoflush(1);
$s->blocking(0);
$s->print("terminate\t0\n");

$dp->exit_is( 0 );

done_testing;