#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use lib 't/lib';
use Test::DaemonProxy;
use File::Spec::Functions;

my $dp= Test::DaemonProxy->new;
my $localpath= catdir($dp->temp_path, '070-chdir');
`mkdir -p "$localpath"`;
`rm -r "$localpath"`;
is( $?, 0, "wiped $localpath" );
mkdir($localpath) or die "$!";

$dp->run('-i');
$dp->send('chdir', $localpath);
$dp->send('fd.open', 'test', 'write,create', $$);
$dp->sync;

ok( -f catfile($localpath, $$), 'created file in new cur-dir' );

$dp->send('terminate', 0);
$dp->exit_is( 0 );

done_testing;