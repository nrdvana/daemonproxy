#! /usr/bin/env perl

use strict;
use warnings;

my @commands;
my @functions;
my @hashcodes;

while (<STDIN>) {
	if ($_ =~ m|COMMAND\s*\(\s*"(\S+)"\s*,\s*(\S+)\s*\)|) {
		push @commands, $1;
		push @functions, $2;
	}
}

printf "%d commands\n", scalar @commands;

# table size is 1.5 x number of entries rounded up to power of 2.
my $mask= int(1.5 * @commands);
$mask |= $mask >> 1;
$mask |= $mask >> 2;
$mask |= $mask >> 4;
$mask |= $mask >> 8;
$mask |= $mask >> 16;
my $table_size= $mask+1;

print "table size is $table_size\n";

sub hash_fn {
	my ($string, $shift)= @_;
	my $result= 0;
	$result= ($result << $shift) ^ (($result >> 24)&0xFF) ^ $_
		for unpack( 'C' x length($string), $string );
	$result;
}

sub print_table {
	my ($table, $shift, $mul)= @_;
	print "int ctl_command_hash_func(const char* buffer) {\n"
		. "	int x= 0;\n"
		. "	const char *p= buffer;\n"
		. "	while (*p && *p != '\t') {\n"
		. "		x= (x << $shift) ^ ((x >> 24) & 0xFF) ^ (*p & 0xFF);\n"
		. "	}\n"
		. "	return (x * $mul) & $mask;\n"
		. "}\n"
		. "\n"
		. "ctl_command_table_entry_t ctl_command_table[]= {\n";
	for (my $i= 0; $i < $table_size; $i++) {
		if ($table->[$i] >= 0) {
			print '	{"'.$commands[$table->[$i]].'",'.$functions[$table->[$i]]."},\n";
		} else {
			print '	{"", NULL},'."\n";
		}
	}
	print "	{NULL, NULL}\n};\n";
}

# pick factors for the hash function until each has a unique code
my $shift;
my $mul;
mul_loop: for ($mul= 1; $mul < 1000000; $mul += 2) {
	shift_loop: for ($shift= 1; $shift < 15; $shift++) {
		my @table= (-1) x @commands;
		for (my $i= 0; $i < @commands; $i++) {
			my $bucket= hash_fn($commands[$i], $shift, $mul) & $mask;
			next shift_loop if $table[$bucket];
			$table[$bucket]= $i;
		}
		print_table(\@table, $shift, $mul);
		exit 0;
	}
}
die "No value of \$shift / \$mul results in unique codes for each command\n";

