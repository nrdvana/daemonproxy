#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use Socket;
use IO::Select;
use Cwd 'abs_path';

my $dp= Test::DaemonProxy->new;
my $path= $dp->temp_path . '/081-close.sock';
$dp->run('-i', '-S', $path);
$dp->send('echo', 'ready');
$dp->recv_ok(qr/^ready$/m);

sub connect_client {
    socket(my $sock, PF_UNIX, SOCK_STREAM, 0) or die "socket: $!";
    connect($sock, sockaddr_un($path)) or die "connect: $!";
    return $sock;
}

sub read_to_eof {
    my ($sock)= @_;
    my $result= '';
    while (IO::Select->new($sock)->can_read(2)) {
        my $n= sysread($sock, my $chunk, 4096);
        return $result if defined($n) && !$n;
        die "read: $!" unless defined $n;
        $result .= $chunk;
    }
    fail('socket reaches EOF');
    return $result;
}

for my $finish ('half-close', 'exit') {
    my $sock= connect_client();
    my $command= "echo\tbefore-$finish\n";
    $command .= "exit\n" if $finish eq 'exit';
    syswrite($sock, $command) == length($command) or die "write: $!";
    shutdown($sock, SHUT_WR) or die "shutdown: $!" if $finish eq 'half-close';
    is(read_to_eof($sock), "before-$finish\n", "$finish flushes queued reply");
    close($sock);
    $dp->send('echo', 'alive');
    $dp->recv_ok(qr/^alive$/m, "stdio survives $finish");
}

# Force descriptor reuse after an abruptly disconnected controller.
my $sock= connect_client();
close($sock);
$dp->send('echo', 'after-disconnect');
$dp->recv_ok(qr/^after-disconnect$/m);
$sock= connect_client();
syswrite($sock, "echo\treconnected\nexit\n");
is(read_to_eof($sock), "reconnected\n", 'new socket works after disconnect');
close($sock);
$dp->send('socket.create', '-', '');
$dp->recv_ok(qr/^error\tFailed to create control socket/m, 'empty socket path rejected');
$sock= connect_client();
syswrite($sock, "echo\tstill-listening\nexit\n");
is(read_to_eof($sock), "still-listening\n", 'invalid path preserves listener');
close($sock);

my $regular= "$path.file";
open(my $file, '>', $regular) or die "open: $!";
print {$file} "preserve me\n";
close($file);
$dp->send('socket.create', '-', $regular);
$dp->recv_ok(qr/^error\tFailed to create control socket/m, 'regular file is not replaced');
open($file, '<', $regular) or die "regular file was removed: $!";
is(scalar <$file>, "preserve me\n", 'regular file contents preserved');
close($file);
my $link= "$path.link";
symlink(abs_path($regular), $link) or die "symlink: $!";
$dp->send('socket.create', '-', $link);
$dp->recv_ok(qr/^error\tFailed to create control socket/m, 'symlink is not replaced');
ok(-l $link, 'symlink preserved');
unlink($link, $regular);
$dp->send('terminate', 0);
$dp->exit_is(0);
unlink($path);
done_testing;
