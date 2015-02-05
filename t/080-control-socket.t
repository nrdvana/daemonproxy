#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
my $sockpath= $dp->temp_path . '/080-tempfile.sock';
$dp->run('-i', '-S', $sockpath);
$dp->send('echo', 'foo');
$dp->recv_ok( qr/^foo$/m, 'command over stdio' );

use Socket;
socket(my $s, PF_UNIX, SOCK_STREAM, 0) || die "socket: $!";
connect($s, sockaddr_un($sockpath)) || die "connect: $!";
$s->autoflush(1);
$s->blocking(0);

$s->print("echo	testing\n");
$dp->discard_response;
my $line= <$s>;
is( $line, "testing\n", "recv echo via unix socket" );

$s->print("terminate	0\n");
$dp->discard_response;
$dp->exit_is( 0 );

unlink $sockpath;

done_testing;