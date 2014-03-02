#! /usr/bin/perl

=head1 DESCRIPTION

Generates tarball containing everything needed to build
daemonproxy *without* perl scripts.

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

use strict;
use warnings;

my $proj_root= $ENV{PROJ_ROOT};

my @distclean= (
	'.git',
	'build',
	'scripts/dev_rules.mak',
	'scripts/autom4te.cache',
);

my @uncommitted= `git --git-dir="$proj_root/.git" status --porcelain`;
!@uncommitted
	or die "Uncommitted git changes!";

-d "$proj_root/src" and -d "$proj_root/scripts"
	or die "Incorrect project root \"$proj_root\"";

my $dest_dir= "$proj_root/dist/next";
system('mkdir', '-p', $dest_dir) == 0
and system('rm', '-rf', $dest_dir) == 0
or die "Can't set up $dest_dir";

system('git', 'clone', $proj_root, $dest_dir) == 0
or die "Can't clone project into $dest_dir";

$ENV{MAKE_DIST}= 1;

system('make', '-C', $dest_dir) == 0
or die "Make failed";

system('make', '-C', $dest_dir, 'test') == 0
or die "Testing failed";

system('rm', '-rf', map { "$dest_dir/$_" } @distclean) == 0
or die "Unable to remove temp git dir";

# Extract version for directory name

my $canonical= `sed -n '/canonical=/s/.*=//p' '$dest_dir/src/version_data.autogen.c'`;
chomp($canonical);
$canonical =~ /^\d+\.\d+(\.\d+)$/
	or die "Version number seems wrong: '$canonical'";

my $distname= "daemonproxy-$canonical";
my $dest_renamed= "$proj_root/dist/$distname";
system('rm', '-rf', $dest_renamed, "$proj_root/dist/$distname.tar.bz2");
system('mv', $dest_dir, $dest_renamed) == 0
or die "Unable to rename $dest_dir to $dest_renamed";

system('tar', '-C', "$proj_root/dist", '-cjf', "$proj_root/dist/$distname.tar.bz2", $distname) == 0
or die "Unable to create tar archive";

print "\nBuilt $proj_root/dist/$distname.tar.bz2\n\n";
