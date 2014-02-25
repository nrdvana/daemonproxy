#! /usr/bin/env perl

=head1 DESCRIPTION

Generates tables of signals from list of signal names.

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

use strict;
use warnings;

print <<___;

const struct sig_list_item sig_list[]= {
___

my $buf= "\\0\\0\\0\\0\\0\\0\\0\\0";
while (<STDIN>) {
	my ($name)= ($_ =~ /^SIG(\w+)/)
		or next;
	length($name) < 8 or die "Signal name length exceeds limit: SIG$name\n";
	my $padded= $name . substr($buf, length($name)*2);
	print <<___;
#ifdef SIG$name
	{ SIG$name, { "$padded" } },
#endif
___
}

print <<___;
	{ 0, { "\\0\\0\\0\\0\\0\\0\\0\\0" } }
};

___
