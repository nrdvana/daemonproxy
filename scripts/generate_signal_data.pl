#! /usr/bin/env perl

=head1 DESCRIPTION

Generates tables of signal names and numbers using output of C preprocessor
on a specially crafted header containing a list of all signals.

=head1 COPYRIGHT

Copyright (C) 2014 Michael Conrad <mike@nrdvana.net>

Distributed under GPLv2, see LICENSE

=cut

use strict;
use warnings;

my @table= ();
while (<STDIN>) {
	next unless $_ =~ /^"SIG(\w+)"=(\d+)/;
	length($1) < 8 or die "Signal name length exceeds limit: SIG$1\n";
	$2 > 0 or die "Signal value is not positive: SIG$1=$2\n";
	push @table, [ $1, $2 ];
}

print <<___;

const struct sig_list_item sig_list[]= {
___

my $buf= "\\0\\0\\0\\0\\0\\0\\0\\0";
for (@table) {
	my ($name, $num)= @$_;
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
