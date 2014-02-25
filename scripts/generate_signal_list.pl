#! /usr/bin/env perl

=head1 DESCRIPTION

Generates special header file to be passed through C preprocessor
and used as input for generate_signal_data.pl

Obtains SIG* names from man page.  (yes, lame)

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

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
