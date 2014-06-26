#! /usr/bin/env perl
use strict;
use warnings;
use Try::Tiny;
use Log::Any '$log';

$|= 1;

my $i= 0;
sub read_events {
	++$i;
	print "echo\t----$i----\n";
	while (next_event() ne "----$i----") {}
}

my %service;

sub next_event {
	my $timeout= shift;
	my $event= '';
	try {
		local $SIG{ALRM}= sub { die "timeout\n"; };
		alarm $timeout if defined $timeout;
		$event= <STDIN>;
		alarm 0 if defined $timeout;
	};
	exit 2 unless defined $event;
	return unless length $event;
	chomp $event;
	my @args= split /\t/, $event;
	$log->debug("got event ".join(',', @args));
	my $event_name= shift @args;
	$event_name =~ tr/./_/;
	my $m= __PACKAGE__->can('event_'.$event_name);
	$m->(@args) if $m;
	return $event_name;
}

sub event_service_state {
	my ($name, $state, $timestamp, $pid, $exitreason, $exitvalue, $uptime, $downtime)= @_;
	$service{$name}{state}= $state;
	$service{$name}{pid}= $pid;
}

sub event_service_auto_up {
	my ($name, $restart_interval, @triggers)= @_;
	$service{$name}{auto_up}= \@triggers;
	$service{$name}{restart_interval}= $restart_interval;
}

sub live_services {
	return grep {
			($service{$_}{state}||'') ne 'down'
			and $service{$_}{pid}+0 != $$
		} keys %service;
}

sub kill_batch {
	my ($signal, $timeout, @list)= @_;
	for (@list) {
		$log->info("sending $signal to $_");
		print "service.signal	$_	$signal\n";
	}
	try {
		local $SIG{ALRM}= sub { die "timeout\n"; } if $timeout;
		alarm $timeout if $timeout;
		while (@list) {
			next_event();
			@list= grep { $service{$_}{state} ne 'down' } @list;
		}
		alarm 0;
	};
	return @list == 0;
}

$log->info("reading daemonproxy state");
print "statedump\n";
read_events();

$log->info("disabling auto_up for all services");
for (keys %service) {
	print "service.auto_up	$_	-\n";
}
read_events();

$log->info("Sending TERM to all non-logger services");
unless (kill_batch('TERM', 10, grep { !/log/ } live_services())) {
	my @live= grep { !/log/ } live_services();
	$log->error("not all services exited on SIGTERM: ".join(', ', @live));
	kill_batch('KILL', 5, @live);
}

# Now kill the loggers
$log->info("Sending TERM to logger services");
unless (kill_batch('TERM', 5, grep { /log/ } live_services())) {
	my @live= grep { /log/ } live_services();
	$log->error("not all services exited on SIGTERM: ".join(', ', @live));
	kill_batch('KILL', 5, @live);
}

my @live= live_services();
$log->error("The following services could not be killed: ".join(', ', @live));

my $exit= @live? 2 : 0;
print "terminate	$exit\n";
