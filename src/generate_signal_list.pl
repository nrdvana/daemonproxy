#! /usr/bin/env perl
use strict;
use warnings;

my $signal_manpage= `man 7 signal`;
$? == 0 or die "Unable to read manpage for signals";

my %seen= ();
print "#include <signal.h>\n";
for ($signal_manpage =~ /\b(SIG\w+)/g) {
	print qq{"$_"=$_\n}
		unless $seen{$_}++;
}
