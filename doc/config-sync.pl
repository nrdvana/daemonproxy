#! /usr/bin/env perl

=head1 DESCRIPTION

"I want a config file that is easy to edit, and then send SIGHUP to
daemonproxy to have it reload the config file."

This is a simple controller script which accomplishes that.  Start this
controller script on startup, and on receipt of SIGHUP, for example with
the following daemonproxy commands:

  service.args	controller	config-sync.pl	/path/to/config.yaml
  service.fds	controller	control.event	control.cmd	stderr
  service.auto_up	controller	5	SIGHUP
  service.start	controller

The yaml file you give to the script might look like this:

  services:
    my-service:
      run: "./daemon --with arguments"
      log: "tinylog --args --more-args"
    my-other-service:
      run: ["./some-other-daemon", "with", "arguments"]
      # no logger for this one

=cut

use strict;
use warnings;
use YAML ();

$|=1; # immediate flush on STDOUT

my %service;
my %handle;

sub main {
	my $cfgfile= shift;
	
	# clear one occurrence of SIGHUP in daemonproxy, so it doesn't
	# restart us infinitely
	print "signal.clear\tSIGHUP\t1\n";
	
	# now load our yaml config file
	defined $cfgfile or die "Missing argument of YAML file";
	my $config= validate_config(YAML::LoadFile($cfgfile));

	# calculate what we want the new services to look like
	my %new_service= ();
	for my $svname (sort keys %{$config->{services}}) {
		my $svcfg= $config->{services}{$svname};
		$new_service{$svname}{args}= $svcfg->{run};
		if ($svcfg->{log}) {
			$new_service{$svname}{fds}= [ 'null', $svname.'-log.w', $svname.'-log.w' ];
			$new_service{$svname.'-log'}{args}= $svcfg->{log};
			$new_service{$svname.'-log'}{fds}= [ $svname.'-log.r', 'stdout', 'stdout' ];
			$new_service{$svname.'-log'}{triggers}= [ 'always' ];
			$new_service{$svname.'-log'}{restart_interval}= 2;
		}
		else {
			$new_service{$svname}{fds}= [ 'null', 'stdout', 'stdout' ];
		}
		$new_service{$svname}{triggers}= [ 'always' ];
		$new_service{$svname}{restart_interval}= 2;
	}

	# fetch state from daemonproxy
	print "statedump\n";
	read_events();
	
	# resolve discrepencies between %services and %new_services
	my @down_service;
	my @up_service;
	my @rm_service;
	for my $svname (sort keys %{{ %services, %new_services }}) {
		if (!defined $services{$svname}) {
			# create pipe if it doesn't exist
			...;
			# create and start service $svname
			push @up_service, $svname;
		}
		elsif (!defined $new_services{$svname}) {
			# stop and remove service $svname
			push @down_service, $svname;
			push @rm_service, $svname;
		}
		else {
			# compare arguments and fds.  If changed, update and restart
			...;
			if (...) {
				push @down_service, $svname;
				push @up_service, $svname;
			}
		}
	}
	...; # diagnostic messages
	for (sort { ... } @down_service) {
		print_cmd('service.auto_up', $_, '-');
		print_cmd('service.signal', $_, 'SIGTERM');
		# wait briefly for it to go down.  In parallel if possible...
		#  and try to stop non-loggers before loggers.
		...
	}
	...; # diagnostic messages
	for (@rm_service) {
		print_cmd('service.delete', $_);
	}
	...; # diagnostic messages
	for (@up_service) {
		print_cmd('service.args', $svname, @{ $new_services{$_}{args} });
		print_cmd('service.fds',  $svname, @{ $new_services{$_}{fds} });
		print_cmd('service.auto_up', $_, $new_services{$_}{restart_interval}, @{ $new_services{$_}{triggers} });
	}
}

sub validate_config {
	my $cfg= shift;
	defined $cfg and ref $cfg eq 'HASH'
		or die "Config should be a name/value map at the top level\n";
	my %tmp= %$cfg;
	my $svcs= delete $tmp{services};
	!keys %tmp
		or die "unknown section '".(keys %tmp)[0]."' in config file at top level\n";
	defined $svcs and ref $svcs eq 'HASH'
		or die "services should be a name/value map\n";
	for my $svname (keys $svcs) {
		$svname =~ /[-A-Za-z0-9_.]/
			or die "Invalid service name '$svname'\n";
		$svname =~ /-log$/
			and die "Service name '$svname' conflicts with auto-generated logger names\n";
		my $svc= $svcs->{$svname};
		defined $svc and ref $svc eq 'HASH'
			or die "Service definition '$svname' must be a key/value map\n";
		defined $svc->{run}
			or die "Service '$svname' has no 'run' key\n";
		$svc->{run}= validate_runargs($svc->{run});
		$svc->{log}= validate_runargs($svc->{log})
			if defined $svc->{log};
	}
	return $cfg;
}

sub validate_runargs {
	my ($svname, $field, $args)= @_;
	# Run string can either be shell code, or argument list
	if (!ref $args) {
		# If it is shell code, warn if they forgot to 'exec'
		$args =~ /exec/
			or warn "Service '$svname' runs shell code that does not 'exec'\n";
		$args= [ 'sh', '-c', $args ];
	}
	ref $args eq 'ARRAY'
		or die "Expected shell string or argument array for '$svname' '$field'\n";
	for (@$args) {
		$_ =~ /[\t\n]/
			and die "Service '$svname' has a literal TAB or LF in its shell code.  Try converting to a script file.\n";
	}
	return $args;
}

my $i= 0;
sub read_events {
	++$i;
	print "echo\t----$i----\n";
	while (next_event() ne "----$i----") {}
}

sub print_cmd {
	print join("\t", @_)."\n";
}

sub next_event {
	my $line= <STDIN>;
	defined $line or exit 2;          # exit on EOF
	chomp $line;
	return '' unless length $line;    # only continue if have a name
	my ($name, @args)= split(/\t/, $event); # split on tab
	my $dispatch= "handle_event_$name";
	$dispatch =~ s/\./_/g;
	my $fn= main->can($dispatch);
	$fn->(@args) if $fn;               # dispatch to relevant method if exists
	return $name;
}

sub handle_event_fs_state {
	my $fdname= shift;
	@{$handle{$fdname}}{'type','flags','descrip'}= @_;
}

sub handle_event_service_state {
	my $svname= shift;
	@{$service{$svname}}{'state','timestamp','pid','exitreason','exitvalue','uptime','downtime'}= @_;
}

sub handle_event_service_args {
	my ($svname, @args)= @_;
	$service{$svname}{args}= \@args;
}

sub handle_event_service_fds {
	my ($svname, @fds)= @_;
	$service{$svname}{fds}= \@fds;
}

sub handle_event_service_auto_up {
	my ($svname, $restart_interval, @triggers)= @_;
	$service{$svname}{triggers}= \@triggers;
	$service{$svname}{restart_interval}= $restart_interval;
}

main();