#! /usr/bin/env perl

=head1 DESCRIPTION

Generates version.autogen.c from the version number in ChangeLog, the hostname,
and the git HEAD.

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

use strict;
use warnings;

my $is_dist=   $ENV{MAKE_DIST};
my $proj_root= $ENV{PROJ_ROOT};

-d "$proj_root/src" and -d "$proj_root/scripts"
	or die "Incorrect project root \"$proj_root\"";

# Version major.minor.release comes from ChangeLog

my ($major, $minor, $release, $suffix);

open CHANGELOG, '<', "$proj_root/ChangeLog" or die "Can't open ChangeLog";
while (<CHANGELOG>) {
	next unless $_ =~ /^\S.*Version (\d+)\.(\d+)(?:\.(\d+))?/;
	($major, $minor, $release)= ($1, $2, $3 || 0);
	last;
}
close CHANGELOG;
defined $major or die "Can't find version in ChangeLog";

# If making a version.c for distribution, we leave out the suffix

sub hostname { my $x= `hostname`; chomp $x; $? == 0? $x : 'unknown' }
$suffix= $is_dist? '' : '-' . hostname();

# The build timestamp always comes from the Makefile as a preprocessor value.

# Find the HEAD from git

my $HEAD= `git --git-dir='$proj_root/.git' log -n 1`;
$? == 0 or die "Can't run 'git log'";
$HEAD =~ /commit (\S+)/ or die "Can't determine commit from: \"$HEAD\"\n";
my $commit= $1;

# Is working dir clean?

my $changes= `git --git-dir='$proj_root/.git' status --porcelain`;
$? == 0 or die "Can't run 'git status'";
my $dirty= length($changes) > 2? 'true' : 'false';

# Build version.c

sub c_str { 
	my $str= shift;
	$str=~ s/[\x00-\x19"'\\]/ sprintf("0x%02X", ord($_)) /eg;
	return qq{"$str"};
}

my $suffix_c_str= c_str($suffix);
my $git_c_str= c_str($commit);
my $canonical= "$major.$minor.$release$suffix";

print <<END;
#include "config.h"

/*
canonical=$canonical
*/
const int     version_major=     $major;
const int     version_minor=     $minor;
const int     version_release=   $release;
const char *  version_suffix=    $suffix_c_str;
const time_t  version_build_ts=  CURRENT_UNIX_TIMESTAMP;
const char *  version_git_head=  $git_c_str;
const bool    version_git_dirty= $dirty;
END
