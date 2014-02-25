#! /usr/bin/env perl

=head1 DESCRIPTION

Generates tables of commandline options from POD comments in C source.

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

use strict;
use warnings;

my @opts;
my @opts_building; # short_opt, long_opt, arg_name, fn_name, desc
my $pod;
while (<STDIN>) {
	# Look for option documentation items
	if ($_ =~ /^=item -(\w)(?:\s+(\S+))?/) {
		if (!@opts_building || defined $opts_building[-1][0]) {
			push @opts_building, [ $1, undef, $2, undef, undef ];
		} else {
			$opts_building[-1][0]= $1;
			$opts_building[-1][2] ||= $2;
		}
		$pod= 1;
	}
	elsif ($_ =~ /^=item --(\w[\w-]*)(?:\s+(\S+))?/) {
		if (!@opts_building || defined $opts_building[-1][1]) {
			push @opts_building, [ undef, $1, $2, undef, undef ];
		} else {
			$opts_building[-1][1]= $1;
			$opts_building[-1][2] ||= $2;
		}
		$pod= 1;
	}
	elsif ($_ =~ /^=cut/) {
		$pod= 0;
	}
	elsif ($pod && !defined $opts_building[-1][4] && $_ =~ /^(\w+[^.]+)\./) {
		$opts_building[-1][4]= $1;
	}
	elsif (!$pod && $_ =~ /^void\s+(\w+)\(/) {
		$_->[3]= $1 for @opts_building;
		push @opts, @opts_building;
		@opts_building= ();
	}
}

for (@opts) {
	my ($short, $long, $arg, $fn, $desc)= @$_;
	print "void $fn(char **);\n";
}
print "\n";
print "const struct option_table_entry_s option_table[]= {\n";
for (@opts) {
	my ($short, $long, $arg, $fn, $desc)= @$_;
	defined $long or die "No long option for -$short";
	defined $fn or die "No function found for --$long";
	defined $desc or die "No description for --$long";
	printf qq[	{ %s, %-18s, %-10s, %-20s, "%s" },\n],
		defined $short? "'$short'" : " 0 ", '"'.$long.'"',
		defined $arg? '"'.$arg.'"'  : "NULL", $fn, $desc;
}
print "	{  0 , NULL, NULL, NULL, NULL }\n";
print "};\n";