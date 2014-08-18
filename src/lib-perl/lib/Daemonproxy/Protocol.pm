package Daemonproxy::Protocol;
use Moo;
use Log::Any '$log';
use Time::HiRes 'time';
use Carp;

has 'handle',           is => 'ro', required => 1;
has 'state',            is => 'rw';
has 'pending_commands', is => 'rw';
has 'signals',          is => 'rw';

sub service {
	my ($self, $svcname)= @_;
	return Daemonproxy::Protocol::Service->new(conn => $self, name => $svcname);
}

sub file_descriptor {
	my ($self, $fdname)= @_;
	return Daemonproxy::Protocol::Service->new(conn => $self, name => $fdname);
}

*fd= *file_descriptor;
*svc= *service;

sub pump_events {
	...;
}

sub process_event {
	my ($self, $text)= @_;
	chomp $text;
	my ($event_id, @args)= split /\t/, $text;
	$event_id =~ tr/./_/g;
	if (my $mth= $self->can('process_event_'.$event_id)) {
		$self->$mth(@args);
	} else {
		carp "Unknown event '$text'";
	}
}

sub process_event_service_state {
	my $self= shit;
	my $service_name= shift;
	@{$self->{state}{services}{$service_name}{state}}{qw( state timestamp pid exitreason exitvalue uptime downtime )}=
		map { defined $_ && $_ eq '-'? undef : $_ } @_;
}

sub process_event_service_auto_up {
	my ($self, $service_name, $restart_interval, @triggers)= @_;
	$restart_interval= undef
		unless defined $restart_interval && $restart_interval ne '-';
	@{$self->{state}{services}{$service_name}}{'restart_interval','triggers'}= ($restart_interval, @triggers? \@triggers : undef);
}

sub process_event_service_meta {
	my ($self, $service_name, @meta_flags)= @_;
	$self->{state}{services}{$service_name}{meta}= \@meta_flags;
}

sub process_event_service_fds {
	my ($self, $service_name, @fds)= @_;
	$self->{state}{services}{$service_name}{fds}= \@fds;
}

sub process_event_fd_state {
	my ($self, $fd_name, $type, $flags, $descrip)= @_;
	@{$self->{state}{handles}{$fd_name}{state}}{'type','flags','descrip'}= ($type, $flags, $descrip);
}

sub process_event_echo {
	my ($self, undef, @args)= @_;
	if (@args && $args[0] eq '--cmd-complete--') {
		defined $args[1] or croak "No command id in --cmd-complete-- event";
		my $cmd= delete $self->{pending_commands}{$args[1]};
		$cmd->complete(1) if $cmd;
	}
}

sub send {
	my $self= shift;
	my $msg= join("\t", @_);
	$self->handle->print($msg."\n");
}

...; XXX
sub begin_cmd {
	&{ $self->can('send') };
	&{ $self->can('_get_cmd_watcher') } if defined wantarray;
}

sub _get_cmd_watcher {
	my $self= shift;
	$self->send(@_) if @_;
	return $self->flush(@_) if (defin
	my $cmd= Daemonproxy::Protocol::Command->new( conn => $self, command => $cmd_id, args => \@args );
	$self->send('echo', '--cmd-complete--', $cmd);
	weaken($self->{pending_commands}{$cmd}= $cmd);
	$cmd;
}
...; XXX

sub reset {
	$self->{state}= {};
	return $self->begin_cmd("statedump");
}

sub flush {
	my ($self)= @_;
	$self->begin_cmd()->wait;
}

package Daemonproxy::Protocol::Service;

sub name { ... }
sub is_running { ... }

sub arguments { ... }
sub arg_list { @{ $_[0]->arguments } }

sub file_descriptors { ... }
sub fd_list  { @{ $_[0]->file_descriptors } }

sub meta { ... }

package Daemonproxy::Protocol::FileDescriptor;

sub is_pipe { ... }
sub is_file { ... }
sub name { ... }

package Daemonproxy::Protocol::Command;
use Moo;

has 'conn',     is => 'ro', required => 1;
has 'command',  is => 'ro', required => 1;
has 'args',     is => 'ro', required => 1;
has 'complete', is => 'rw';

sub wait {
	$self->conn->pump_events until $self->complete;
}

1;