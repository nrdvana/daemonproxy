#! /usr/bin/env perl
use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->run('-i', '-v');
# This fits the command buffer but overflows logging once its prefix is added.
$dp->send('echo', 'x' x 1000);
$dp->recv_stdout_ok(qr/^x{1000}$/m, 'long command still executes');
$dp->recv_stderr_ok(qr/^warning: lost \d+ log messages$/m, 'dropped log messages reported');
$dp->send('echo', 'after-log-overflow');
$dp->recv_stdout_ok(qr/^after-log-overflow$/m, 'commands continue after overflow');
$dp->recv_stderr_ok(qr/^debug: .*after-log-overflow/m, 'subsequent message is logged');
$dp->send('terminate', 0);
$dp->exit_is(0);
done_testing;
