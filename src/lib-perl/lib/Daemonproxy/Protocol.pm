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
	return bless [$self, $svcname], 'Daemonproxy::Protocol::Service';
}

sub file_descriptor {
	my ($self, $fdname)= @_;
	return bless [$self, $fdname], 'Daemonproxy::Protocol::Service';
}

*fd= *file_descriptor;
*svc= *service;

sub pump_events {
	my $self= shift;
	while (defined my $line= $self->handle->getline) {
		$self->process_event($line);
	}
}

sub process_event {
	my ($self, $text)= @_;
	chomp $text;
	my ($event_id, @args)= split /\t/, $text;
	$event_id =~ tr/./_/g;
	if (my $mth= $self->can('process_event_'.$event_id)) {
		$self->$mth(@args);
	} else {
		$log->warn("Unknown event '$text'");
	}
}

sub process_event_service_state {
	my $self= shit;
	my $service_name= shift;
	@{$self->{state}{services}{$service_name}}{qw( state timestamp pid exit_reason exit_value uptime downtime )}=
		map { defined $_ && $_ eq '-'? undef : $_ } @_;
	delete $self->{state}{services}{$service_name}
		if $self->{state}{services}{$service_name}{state} eq 'deleted';
}

sub process_event_service_auto_up {
	my ($self, $service_name, $restart_interval, @triggers)= @_;
	$restart_interval= undef
		unless defined $restart_interval && $restart_interval ne '-';
	@{$self->{state}{services}{$service_name}}{'restart_interval','triggers'}= ($restart_interval, @triggers? \@triggers : undef);
}

sub process_event_service_tags {
	my ($self, $service_name, @tags)= @_;
	$self->{state}{services}{$service_name}{'tags','tags_hash'}= (\@tags, undef);
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

sub begin_cmd {
	my $self= shift;
	$self->send(@_);
	$self->_get_cmd_watcher(@_) if defined wantarray;
}

sub _get_cmd_watcher {
	my $self= shift;
	my $cmd= Daemonproxy::Protocol::Command->new( conn => $self, command => shift, args => [ @_ ] );
	$self->send('echo', '--cmd-complete--', $cmd);
	weaken($self->{pending_commands}{$cmd}= $cmd);
	return $cmd;
}

sub reset {
	$self->{state}= {};
	return $self->begin_cmd("statedump");
}

sub flush {
	my ($self)= @_;
	$self->_get_cmd_watcher()->wait;
}

package Daemonproxy::Protocol::Service;
use strict;
use warnings;
no warnings 'uninitialized';

sub conn { $_[0][0] }
sub name { $_[0][1] }

sub _svc { $_[0][0]->{state}{services}{$_[0][1]} || {} }

sub state {
	return $_[0]->_svc->{state};
}

sub is_running {
	return $_[0]->state eq 'up';
}

sub is_starting {
	return $_[0]->state eq 'starting';
}

sub pid {
	return $_[0]->_svc->{pid};
}

sub exit_reason {
	return $_[0]->_svc->{exitreason};
}

sub exit_value {
	return $_[0]->_svc->{exit_value};
}

sub arg_list { @{ $_[0]->_svc->{args} } }
sub fd_list  { @{ $_[0]->_svc->{fds} } }
sub tag_list { @{ $_[0]->_svc->{tags} } }

sub arguments        { [ $_[0]->arg_list ] }
*args = *arguments;
sub file_descriptors { [ $_[0]->fd_list ] }
*fds = *file_descriptors;
sub tags             { [ $_[0]->tag_list ] }

sub _tag_hash {
	my $svc= (shift)->_svc;
	$svc->{tags_hash} ||= { map { $_ =~ /^([^=]*)(?:=(.*))?/? ($1 => defined $2? $2 : 1) : () } @{ $svc->{tags} } };
}

sub tag_values {
	my $tag_hash= shift->_tag_hash;
	return @_ > 1? @{$tag_hash}{@_}
		: ref $_[0] eq 'ARRAY'? [ @{$tag_hash}{@{$_[0]}} ]
		: $tags_hash->{$_[0]};
}

sub start {
	my $self= shift;
	$self->conn->begin_cmd('service.start', $self->name);
}

sub signal {
	my ($self, $signal)= @_;
	$self->conn->begin_cmd('service.signal', $self->name, $signal);
}

sub signal_pgroup {
	my ($self, $signal)= @_;
	$self->conn->begin_cmd('service.signal', $self->name, $signal, 'group');
}

sub delete {
	my $self= shift;
	$self->conn->begin_cmd('service.delete', $self->name);
}

sub set_args {
	my $self= shift;
	my $ret= $self->conn->begin_cmd('service.args', @_);
	# Apply changes immediately, to prevent confusion
	$self->conn->state->{services}{$self->name}{args}= [ @_ ];
}

sub set_fds {
	my $self= shift;
	my $ret= $self->conn->begin_cmd('service.fds', @_);
	# Apply changes immediately, to prevent confusion
	$self->conn->state->{services}{$self->name}{fds}= [ @_ ];
}

sub set_tags {
	my $self= shift;
	my $ret= $self->conn->begin_cmd('service.tags', @_);
	# Apply changes immediately, to prevent confusion
	$self->conn->state->{services}{$self->name}{tags}= [ @_ ];
}

sub set_tag_values {
	my $self= shift;
	my $tag_hash= $self->_tag_hash;
	my @todo= @_;
	@todo= %{ $todo[0] } if @todo == 1 && ref $todo[0] eq 'HASH';
	while (@todo) {
		my ($k, $v)= (shift @todo, shift @todo);
		...;
	}
	...;
}

package Daemonproxy::Protocol::FileDescriptor;
use strict;
use warnings;
no warnings 'uninitialized';

has 'conn', is => 'ro';
has 'name', is => 'ro';

sub is_pipe { ... }
sub is_file { ... }

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