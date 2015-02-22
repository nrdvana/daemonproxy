#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $dp= Test::DaemonProxy->new;
$dp->timeout(0.5);
$dp->run('-i', '-vv');

my $fname= $dp->temp_path . '/280-logdata.txt';
unlink $fname;

$dp->send('fd.open', 'logfile', 'write,create', $fname);
$dp->recv_ok( qr/^fd.state	logfile	.*write/m, 'logfile created' );

$dp->send('log.dest', 'fd', 'logfile');
$dp->send('echo', 'done');
$dp->recv_ok( qr/(.*)^done$/m, 'log.dest finished' );
ok( !($dp->last_captures->[0] =~ /^error/m), 'log.dest successful' );

$dp->send('echo', 'testing');
$dp->recv( qr/^testing$/m ); # wait for completion

# We should now have at least one "debug" logging command in the logfile
my $file_data= do { my $f; open($f, '<', $fname) or die; local $/= undef; <$f> };

like( $file_data, qr/^debug: /m, 'logfile contains daemonproxy log messages' );

# Now redirect to a pipe
$dp->send('fd.pipe', 'log.r', 'log.w');
$dp->send('log.dest', 'fd', 'log.w');

# connect a script to the pipe to verify it contains log messages
my $script= '$i= 0; while (<STDIN>) { if ($_ =~ /^debug: /) { exit 0 } elsif ($i++ > 50) { exit 1 } }';
$dp->send('service.args', 'check_pipe', 'perl', '-e', $script);
$dp->send('service.fds',  'check_pipe', 'log.r', 'stderr', 'stderr');

# and check it
$dp->send('service.start', 'check_pipe');
$dp->recv_ok( qr/^service.state	check_pipe	up.*/m, 'check_pipe started' );
$dp->recv_ok( qr/^service.state	check_pipe	down.*exit	0/m, 'check_pipe sees debug data' );

# Now direct logging to a FD that doesn't exist yet
$dp->send('log.dest', 'fd', 'log2.w');

# Have the service read from this non-existent pipe
$dp->send('service.fds', 'check_pipe', 'log2.r', 'stderr', 'stderr');

# Now create the pipe
$dp->send('fd.pipe', 'log2.r', 'log2.w');

# and check it again
$dp->send('service.start', 'check_pipe');
$dp->recv_ok( qr/^service.state	check_pipe	up.*/m, 'check_pipe started' );
$dp->recv_ok( qr/^service.state	check_pipe	down.*exit	0/m, 'check_pipe sees debug data' );

$dp->terminate_ok;

done_testing;
