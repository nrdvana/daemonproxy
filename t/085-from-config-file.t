#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use lib "$FindBin::Bin/lib";
use Test::DaemonProxy;

my $tempdir= sprintf("%s/tmp/t%03d", $FindBin::Bin, do { $FindBin::Script =~ /(\d+)/? $1 : $$ });
system('mkdir','-p',$tempdir) == 0 or die;
system('rm','-r',$tempdir) == 0 or die;
system('mkdir','-p',$tempdir) == 0 or die;

my $conf1= "$tempdir/conf1";
my $line_too_long= "----------"x 500;
{ open(my $f, '>', $conf1) or die; print $f <<END; }
# comment followed by blank line

nonexistent	command

line overflows buffer $line_too_long
terminate	0
END

my $dp= Test::DaemonProxy->new;
$dp->run('-c', $conf1);
$dp->recv_ok( qr/^error:.*nonexistent.*$/m, 'error reported for nonexistent command' );
$dp->recv_ok( qr/^error:.*command exceeds buffer size.*$/m, 'error reported for long line' );
$dp->exit_is( 0 );

done_testing;
