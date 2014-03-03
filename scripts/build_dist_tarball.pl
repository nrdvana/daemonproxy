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
my $dest_dir= "$proj_root/dist/next";

-d "$proj_root/src" and -d "$proj_root/scripts"
	or die "Incorrect project root \"$proj_root\"";

sub distclean {
	my @distclean= (
		'.git',
		'build',
		'scripts/autom4te.cache',
	);

	system('rm', '-rf', map { "$dest_dir/$_" } @distclean) == 0
	or die "Unable to clean non-distributable files";
}

sub make {
	system('make', '-C', $dest_dir, @_) == 0
	or die "Make failed";
}

# Verify we have all changes checked in

my @uncommitted= `git --git-dir="$proj_root/.git" status --porcelain`;
!@uncommitted
	or die "Uncommitted git changes!";

# Clone project into a temporary directory

system('mkdir', '-p', $dest_dir) == 0
and system('rm', '-rf', $dest_dir) == 0
or die "Can't set up $dest_dir";

system('git', 'clone', $proj_root, $dest_dir) == 0
or die "Can't clone project into $dest_dir";

# Build the generated source files

$ENV{MAKE_DIST}= 1;
make 'autogen_files';

# Remove debug and dev configuration that we added by default

system('sed', '-i', '-e', 's/--enable-dev/--disable-dev/', '-e', 's/--enable-debug/--disable-debug/', "$dest_dir/configure") == 0
or die "Can't remove default configure options";

# Test that we can compile and build it

distclean;

make;
make 'test';

distclean;

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
