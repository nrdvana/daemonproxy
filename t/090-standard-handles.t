#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;
use Time::HiRes 'sleep';

# In interactive mode, stdin and stdout are redirected to /dev/null, but stderr is still usable.

my $dp= Test::DaemonProxy->new;
$dp->run('-i');
my $script= '$|=1; use strict; use warnings; print STDOUT "test\x20message\n"; print STDERR "test\x20message\n"; exit( eof(STDIN)? 42 : 1 );';

$dp->send("service.args", "foo", "perl", "-e", $script);
$dp->send("service.fds",  "foo", "stdin", "stdout", "stderr");
$dp->recv( qr/service.fds/ ); # discard response

$dp->send("service.start", "foo");
$dp->recv_stderr_ok( qr/test message/m, 'got message on stderr' );

ok( ! $dp->recv_stdout( qr/test message/m ), 'no message on stdout' );

$dp->recv_ok( qr/foo.*exit	(\d+)	/m, 'exited' )
	and is( $dp->last_captures->[0], 42, 'stdin at EOF' );

$dp->send("terminate", 0);
$dp->exit_is( 0 );

# When config file on stdin, stdout is still usable

$dp= Test::DaemonProxy->new;
$dp->run('-c', '-');
$script= '$|=1; use strict; use warnings; my $msg= "test message eof is ".(eof(STDIN)?1:0)."\n"; print STDOUT $msg; print STDERR $msg;';

$dp->send("service.args", "foo", "perl", "-e", $script);
$dp->send("service.fds",  "foo", "stdin", "stdout", "stderr");

$dp->send("service.start", "foo");
$dp->recv_stdout_ok( qr/test message eof is (\d)/m, 'got message on stdout' );
$dp->recv_stderr_ok( qr/test message eof is (\d)/m, 'got message on stderr' );
is( $dp->last_captures->[0], 1, 'stdin at EOF' );

$dp->send("terminate", 0);
$dp->exit_is( 0 );

# When no config and not interactive, stdin is available as well

my $sockpath= $dp->temp_path . '/090-socket';
unlink $sockpath;
$dp= Test::DaemonProxy->new;
$dp->run('-S', $sockpath);
$script= '$|=1; use strict; use warnings; my $msg= "test message eof is ".(eof(STDIN)?1:0)."\n"; print $msg; print STDERR $msg; print scalar <STDIN>';

# wait for socket to appear
my $tries= 0;
while (! -S $sockpath && $tries++ < 10) { sleep 0.1; }

use Socket;
socket(my $s, PF_UNIX, SOCK_STREAM, 0) || die "socket: $!";
connect($s, sockaddr_un($sockpath)) || die "connect: $!";
$s->autoflush(1);

$s->print(join("\t", "service.args",  "foo", "perl", "-e", $script)."\n");
$s->print(join("\t", "service.fds",   "foo", "stdin", "stdout", "stderr")."\n");
$s->print(join("\t", "service.start", "foo")."\n");

$dp->send("testing input");
$dp->recv_stdout_ok( qr/test message eof is (\d)/m, 'got message on stdout' );
$dp->recv_stderr_ok( qr/test message eof is (\d)/m, 'got message on stderr' );
is( $dp->last_captures->[0], 0, 'stdin at EOF' );
$dp->recv_stdout_ok( qr/testing input/m, 'received data through stdin' );

$s->print("terminate\t0\n");
$dp->exit_is( 0 );

done_testing;