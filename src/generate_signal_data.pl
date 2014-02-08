#! /usr/bin/env perl
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
const struct sig_list_item {
	int signum;
	union { char chars[8]; int64_t val; } signame;
} sig_list[]= {
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

int sig_num_by_name(strseg_t name) {
	int64_t value;
	int i;
	
	if (name.len > 3 && name.data[0] == 'S' && name.data[1] == 'I' && name.data[2] == 'G') {
		name.data += 3;
		name.len -= 3;
	}
	if (name.len > 8)
		return 0;
	value= 0;
	for (i= name.len-1; i >= 0; i--)
		((char*)&value)[i]= name.data[i];
	for (i= 0; sig_list[i].signum; i++) {
		if (sig_list[i].signame.val == value)
			return sig_list[i].signum;
	}
	return 0;
}

const char* sig_name_by_num(int signum) {
	int i;
	for (i= 0; sig_list[i].signum; i++) {
		if (sig_list[i].signum == signum)
			return sig_list[i].signame.chars;
	}
	return NULL;
}
___
