#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use POSIX ':sys_wait_h';
use File::Temp 'tempfile';
use Time::HiRes 'sleep';

my $harness= Test::DaemonProxy->new;
my $binary= $harness->binary_path;
for my $mask (1..7) {
    my ($config, $path)= tempfile('closed-stdio-XXXX', DIR => $harness->temp_path, UNLINK => 1);
    # The service checks all three named standard handles and ends the supervisor.
    my $script= 'use Fcntl; my $ok=defined(fcntl(STDIN,F_GETFL,0))'
        .'&&defined(fcntl(STDOUT,F_GETFL,0))&&defined(fcntl(STDERR,F_GETFL,0));'
        .'open(my $ctl, ">&=3") or die; print {$ctl} "terminate\t".($ok?0:42)."\n";';
    print {$config} "service.args\tprobe\tperl\t-e\t$script\n",
        "service.fds\tprobe\tstdin\tstdout\tstderr\tcontrol.cmd\n",
        "service.start\tprobe\n";
    close($config) or die "close: $!";
    my $pid= fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        POSIX::close($_) for grep { $mask & (1 << $_) } 0..2;
        exec($binary, '-c', $path) or POSIX::_exit(127);
    }
    my $status;
    for (1..100) {
        if (waitpid($pid, WNOHANG) == $pid) { $status= $?; last; }
        sleep 0.05;
    }
    if (!defined $status) {
        kill 'KILL', $pid;
        waitpid($pid, 0);
        fail("startup with closed handle mask $mask (timed out)");
    }
    else {
        is($status, 0, "startup and service execution with closed handle mask $mask");
    }
    unlink($path);
}
done_testing;
