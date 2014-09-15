package Daemonproxy::Protocol;
use Moo;
use Log::Any '$log';
use Time::HiRes 'time';
use Carp;

has 'rd_handle',        is => 'ro', required => 1;
has 'wr_handle',        is => 'ro', required => 1;
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
	while (defined (my $line= $self->rd_handle->getline)) {
		$self->process_event($line);
	}
}

sub process_event {
	my ($self, $text)= @_;
	chomp $text;
	my ($event_id, @args)= split /\t/, $text;
	$event_id =~ tr/./_/;
	if (my $mth= $self->can('process_event_'.$event_id)) {
		$self->$mth(@args);
	} else {
		$log->warn("Unknown event '$text'");
	}
}

sub process_event_service_state {
	my $self= shift;
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
	@{$self->{state}{services}{$service_name}}{'tags','tags_hash'}= (\@tags, undef);
}

sub process_event_service_fds {
	my ($self, $service_name, @fds)= @_;
	$self->{state}{services}{$service_name}{fds}= \@fds;
}

sub process_event_fd_state {
	my ($self, $fd_name, $type, $flags, $descrip)= @_;
	@{$self->{state}{fds}{$fd_name}{state}}{'type','flags','descrip'}= ($type, $flags, $descrip);
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
	$self->wr_handle->print($msg."\n");
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
	my $self= shift;
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

sub _svc { $_[0][0]->{state}{services}{$_[0][1]} }

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
		: $tag_hash->{$_[0]};
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
	$self->conn->begin_cmd('service.args', $self->name, @_);
}

sub set_fds {
	my $self= shift;
	$self->conn->begin_cmd('service.fds', $self->name, @_);
}

sub set_tags {
	my $self= shift;
	$self->conn->begin_cmd('service.tags', $self->name, @_);
}

sub set_tag_values {
	my $self= shift;
	# since this merges values with existing values, to be safe we need
	# to flush any previous commands to make sure we have the latest tags.
	$self->conn->flush;
	# now perform the merge
	my %tag_hash= %{$self->_tag_hash};
	my @todo= @_;
	@todo= %{ $todo[0] } if @todo == 1 && ref $todo[0] eq 'HASH';
	while (@todo) {
		my ($k, $v)= (shift @todo, shift @todo);
		if (defined $v) {
			$tag_hash{$k}= $v;
		} else {
			delete $tag_hash{$k};
		}
	}
	# build new tags from hash
	$self->set_tags(map { "$_=$tag_hash{$_}" } sort keys %tag_hash);
}

package Daemonproxy::Protocol::FileDescriptor;
use strict;
use warnings;
no warnings 'uninitialized';
use Carp;

sub conn { $_[0][0] }
sub name { $_[0][1] }

sub _fd { $_[0][0]->{state}{fds}{$_[0][1]} }

sub type        { $_[0]->_fd->{type} }
sub flags       { $_[0]->_fd->{flags} }
sub description { $_[0]->_fd->{descrip} }

sub is_pipe    { $_[0]->_fd->{type} eq 'pipe' }
sub is_file    { $_[0]->_fd->{type} eq 'file' }
sub is_special { $_[0]->_fd->{type} eq 'special' }

our %_file_flags= map { $_ => 1 } qw( read write create truncate nonblock mkdir );
sub open_file {
	my ($self, $path, $flags)= @_;
	defined $path or croak "Require file path argument";
	my @flags= !defined $flags? ()
		: !ref $flags? split(/,/, $flags)
		: ref $flags eq 'ARRAY'? @$flags
		: ref $flags eq 'HASH'? (grep { $flags->{$_} } keys %$flags)
		: croak "Can't process flags of $flags";
	$_file_flags{$_} or croak "$_ is not a valid fd.open flag"
		for @flags;
	$self->conn->begin_cmd('fd.open', $self->name, join(',', @flags), $path);
}

sub open_pipe_to {
	my ($self, $dest_name)= @_;
	defined $dest_name
		or croak "Require pipe read-end name";
	$dest_name= $dest_name->name
		if ref $dest_name;
	$self->conn->begin_cmd('fd.pipe', $dest_name, $self->name);
}

sub open_pipe_from {
	my ($self, $src_name)= @_;
	defined $src_name
		or croak "Require pipe write-end name";
	$src_name= $src_name->name
		if ref $src_name;
	$self->conn->begin_cmd('fd.pipe', $self->name, $src_name);
}

sub delete {
	my $self= shift;
	$self->conn->begin_cmd('fd.delete', $self->name);
}

package Daemonproxy::Protocol::Command;
use Moo;

has 'conn',     is => 'ro', required => 1;
has 'command',  is => 'ro', required => 1;
has 'args',     is => 'ro', required => 1;
has 'complete', is => 'rw';

sub wait {
	my $self= shift;
	$self->conn->pump_events until $self->complete;
}

1;