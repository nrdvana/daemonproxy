package Daemonproxy::Protocol;
use Moo;
use Log::Any '$log';
use Time::HiRes 'time';
use Carp;

# ABSTRACT: Helper library for speaking the Daemonproxy protocol

=head1 SYNOPSIS

  # Initialize object, pointing to whatever handles are connected to daemonproxy
  $p= Daemonproxy::Protocol->new(
    rd_handle => \*STDIN,
    wr_handle => \*STDOUT,
  );
  
  # Retrieve daemonproxy's current runtime state.
  $p->sync;
  
  # Dump a list of all running services
  print STDERR "NAME\tPID\n";
  for my $svc ($p->service_list) {
    printf STDERR "%s\t%s\n", $svc->name, $svc->pid
		if $svc->is_running;
  }
  
  # Create a new service
  my $svc= $p->service('mysql');
  $svc->args('runuid', '-s', 'mysql', '/usr/bin/mysqld', '--server-id=2');
  $svc->fds('null', 'stderr', 'stderr');
  $svc->start;
  # delay until daemonproxy acknowledges the commands
  $p->sync;
  # or more conveniently:
  #  $svc->start->wait;
  
  # Create a pipe to a logger
  $p->fd('log.w')->create_pipe_to('log.r');
  my $svc= $p->svc('logger');
  $svc->args('tinylog', '-s', '1000000', '/log/directory');
  $svc->fds('log.r', 'stderr', 'stderr');
  $svc->start;
  
=head1 DESCRIPTION

This module family is a simple set of classes which give you object-oriented
access to the Daemonproxy command and event protocol.  The protocol is already
pretty simple, but using methods on objects can help prevent typos, and there
is always a certain amount of set-up work for any parsing situation, and this
module eases that effort.

For documentation on daemonproxy and its protocol, see the man page.
L<http://www.nrdvana.net/daemonproxy/release/current/daemonproxy.1.html>

=head1 ATTRIBUTES

=head2 rd_handle

The handle to read for daemonproxy events (may be same as wr_handle)

=head2 wr_handle

The handle to write commands to daemonproxy (may be same as rd_handle)

=head2 state

Access to the raw state hierarchy that this module builds while reading
daemonproxy events.  The structure is subject to change in future versions
of this module.

=cut

has 'rd_handle',        is => 'ro', required => 1;
has 'wr_handle',        is => 'ro', required => 1;
has 'state',            is => 'rw';
has 'pending_commands', is => 'rw';

=head1 METHODS

=head2 service

  my $svc= $proto->service( $name );
  -or-
  my $svc= $proto->svc( $name );

Returns a convenience object for accessing a service named C<$name>.

C<$name> does not need to exist; an object will always be returned.
Assigning values to the attributes of an object for a non-existent
service will cause the service to be created.

The object returned will test false in boolean context if its service
doesn't exist yet.

See L<SERVICE OBJECTS>

=head2 file_descriptor

  my $fd= $proto->file_descriptor( $name );
  -or-
  my $fd= $proto->fd( $name );

Returns a convenience object for accessing the descriptor named
C<$name>.  An object is always returned regardless of whether the
file descriptor of this name exists yet.

The object returned will test false in boolean context if its descriptor
doesn't exist yet.

See L<FILE DESCRIPTOR OBJECTS>

=cut

sub service {
	my ($self, $svcname)= @_;
	return bless [$self, $svcname], 'Daemonproxy::Protocol::Service';
}

sub file_descriptor {
	my ($self, $fdname)= @_;
	return bless [$self, $fdname], 'Daemonproxy::Protocol::FileDescriptor';
}

*fd= *file_descriptor;
*svc= *service;

=head2 services

Returns an arrayref of all known services, as objects.  Beware that a service
is not officially known until we see an event from daemonproxy.  See L</sync>.

=head2 service_list

Like L<services>, but returns a list rather than a reference.

=head2 service_names

Like services, but returns an arrayref of strings.

=head2 service_name_list

Like service_names, but returns a list instead of an arrayref.

=cut

sub services {
	return [ (shift)->service_list ];
}
sub service_list {
	my $self= shift;
	return map { $self->service($_) } $self->service_name_list;
}
sub service_names {
	return [ (shift)->service_name_list ];
}
sub service_name_list {
	return keys %{ (shift)->{state}{services} || {} };
}

=head2 file_descriptors

=head2 fds

Returns an arrayref of all known file descriptors, as objects.  Beware that
a file descriptor is not officially known until we see an event from
daemonproxy.  See L</sync>.

=head2 file_descriptor_list

=head2 fd_list

Like L<file_descriptors>, but returns a list rather than an arrayref.

=head2 file_descriptor_names

=head2 fd_names

Like L<file_descriptors>, but returns arrayref of strings.

=head2 file_descriptor_name_list

=head2 fd_name_list

Like L<file_descriptor_names>, but returns a list instead of arrayref.

=cut

sub file_descriptors {
	return [ (shift)->file_descriptor_list ];
}
sub file_descriptor_list {
	my $self= shift;
	return map { $self->fd($_) } $self->file_descriptor_name_list;
}
sub file_descriptor_names {
	return [ (shift)->file_descriptor_name_list ];
}
sub file_descriptor_name_list {
	return keys %{ (shift)->{state}{fds} || {} };
}

*fds= *file_descriptors;
*fd_list= *file_descriptor_list;
*fd_names= *file_descriptor_names;
*fd_name_list= *file_descriptor_name_list;

=head2 pump_events

Call non-blocking readline on the rd_handle over and over
and process each event it returns, using L<process_event>.

=cut

sub pump_events {
	my $self= shift;
	my $prev_block= $self->rd_handle->blocking;
	$self->rd_handle->blocking(0);
	while (defined (my $line= $self->rd_handle->getline)) {
		$self->process_event($line);
	}
	$self->rd_handle->blocking(1) if $prev_block;
}

=head2 process_event

  $proto->process_event( $line_of_text )

Dispatch one event, updating the $proto->state data.

=cut

sub process_event {
	my ($self, $text)= @_;
	chomp $text;
	$log->tracef("process_event %s", $text) if $log->is_trace;
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

sub process_event_service_args {
	my ($self, $service_name, @args)= @_;
	$self->{state}{services}{$service_name}{args}= \@args;
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
	@{$self->{state}{fds}{$fd_name}}{'type','flags','descrip'}= ($type, $flags, $descrip);
}

sub process_event_echo {
	my ($self, undef, @args)= @_;
	if (@args && $args[0] eq '--cmd-complete--') {
		defined $args[1] or croak "No command id in --cmd-complete-- event";
		my $cmd= delete $self->{pending_commands}{$args[1]};
		$cmd->complete(1) if $cmd;
	}
}

=head2 send

  $proto->send( $command, @arguments );

Join the command and arguments and print them to the L<wr_handle>.

=cut

sub send {
	my $self= shift;
	my $msg= join("\t", @_);
	$log->tracef("send_command %s", $msg) if $log->is_trace;
	$self->wr_handle->print($msg."\n");
}

=head2 begin_cmd

  my $promise= $proto->begin_cmd( $command, @arguments );

Like L<send>, but return an object that can be waited on for
completion of the command.

=cut

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

=head2 reset

  $proto->reset;

Re-initialize the $proto->state data structure froma fresh dump
of daemonproxy's state.

=cut

sub reset {
	my $self= shift;
	$self->{state}= {};
	return $self->begin_cmd("statedump");
}

=head2 flush

Block until all commands have been acknowledged by daemonproxy.

=cut

sub flush {
	my ($self)= @_;
	$self->_get_cmd_watcher()->wait;
}

=head2 sync

Either calls L<reset> or L<flush>, as appropriate.  When it returns you know
that daemonproxy has seen all your previous commands and that you have the
latest state events from daemonproxy.

=cut

sub sync {
	my ($self)= @_;
	if (!$self->{state} || !keys %{$self->{state}}) {
		$self->reset->wait;
	} else {
		$self->flush;
	}
}

package Daemonproxy::Protocol::Service;
use strict;
use warnings;
use overload 'bool' => \&exists;
no warnings 'uninitialized';

=head1 SERVICE OBJECTS

Service objects are a simple wrapper around the C<$proto> object and the name
of a service.  More than one object can exist for the same service, and having
the object does not indicate that the service exists on daemonproxy's side.

Service objects support the following attributes/methods:

=head2 conn

The procotol object this refers to.

=head2 name

The service name this refers to.

=cut

sub conn { $_[0][0] }
sub name { $_[0][1] }

sub _svc { $_[0][0]->{state}{services}{$_[0][1]} }

=head2 exists

Whether this service exists on daemonproxy's end.  Beware this is only updated
after a L<sync>.

=head2 state

The string describing the service state.  Undef for nonexistent service, or one
of daemonproxy's documented string values.

=head2 is_running

Whether state is "up"

=head2 is_starting

Whether state is "starting"

=head2 is_reaped

Whether state is "down" and exit_reason is not '-'.

=cut

sub exists {
	defined shift->_svc;
}

sub state {
	return ($_[0]->_svc || {})->{state};
}

sub is_running {
	return $_[0]->state eq 'up';
}

sub is_starting {
	return $_[0]->state eq 'starting';
}

sub is_reaped {
	return $_[0]->state eq 'down' && $_[0]->exit_reason ne '-';
}

=head2 pid

The pid of the process if it has one.  Note that you cannot use this pid directly
without a race condition because daemonproxy might reap the pid at any time.
If you need to send signals to a service, use the 'service.signal' command.

=head2 exit_reason

The string describing why the service exited.  Is '-' until the first time the
service is reaped.  Will then be 'exit' or 'signal'.
Undef if the service doesn't exist.

=head2 exit_value

The integer exit code, or the name of a signal, or '-' until the first time
the service is reaped.  Undef if the service doesn't exist.

=cut

sub pid {
	return ($_[0]->_svc || {})->{pid};
}

sub exit_reason {
	return ($_[0]->_svc || {})->{exitreason};
}

sub exit_value {
	return ($_[0]->_svc || {})->{exit_value};
}

=head2 arguments

=head2 args

Accessor for service's argument list.  Returns an arrayref of the argument strings.
If you supply a new list (or arrayref) this will pass through to L<set_arguments>.

=head2 arg_list

Returns L<arguments> as a list

=head2 set_arguments

=head2 set_args

  $service->set_arguments([ 'ping', '-c', 10, '127.0.0.1' ]);
  $service->set_args( 'ping', '-c', 10, '127.0.0.1' );
  $service->args([ 'ping', '-c', 10, '127.0.0.1' ]);
  $service->set_arguments( @args )->wait();

Assign arguments to a service.
If the service doesn't exist, daemonproxy will create it.
The state of the object will not reflect changes until you call $proto->flush() or
wait() on the returned command object (like in the 4th example).

=cut

sub arguments { @_ > 1? (shift)->set_arguments(@_) : return [ $_[0]->arg_list ] }
*args = *arguments;
sub arg_list  { @{ ($_[0]->_svc || {})->{args} || []} }

sub set_arguments {
	my $self= shift;
	$self->conn->begin_cmd('service.args', $self->name, @_ == 1 && ref $_[0] eq 'ARRAY'? @{$_[0]} : @_);
}
*set_args= *set_arguments;

=head2 file_descriptors

=head2 fds

Accessor for service's file descriptor list.  Return an arrayref of the fd names.
If you supply a new list (or arrayref) this will pass through to L<set_file_descriptors>.

=head2 fd_list

Returns L<file_descriptors> as a list

=head2 set_file_descriptors

=head2 set_fds

  $service->set_fds([ 'null', 'stderr', 'stderr' ]);
  $service->fds(qw( null logger logger ));
  $service->set_file_descriptors( @fds )->wait();

Assign file descriptors to a service.
If the service doesn't exist, daemonproxy will create it.
The state of the object will not reflect changes until you call $proto->flush() or
wait() on the returned command object (like in the 3rd example).

=cut

sub file_descriptors { @_ > 1? (shift)->set_file_descriptors(@_) : return [ $_[0]->fd_list ] }
*fds = *file_descriptors;
sub fd_list  { @{ ($_[0]->_svc || {})->{fds} || []} }

sub set_file_descriptors {
	my $self= shift;
	$self->conn->begin_cmd('service.fds', $self->name, @_ == 1 && ref $_[0] eq 'ARRAY'? @{$_[0]} : @_);
}
*set_fds= *set_file_descriptors;

=head2 tags

Accessor for service's tags.  Returns an arrayref of the tag strings.
If you supply a new list (or arrayref) this will pass through to L<set_tags>.

=head2 tag_list

Returns L<tags> as a list.

=head2 set_tags

  $service->set_tags([ 'user=root', 'session=12345' ]);
  $service->set_tags('user=root', 'session=12345');
  $service->set_tags({ user => 'root', session => 12345 });
  $service->tags(qw( user=root session=12345 ));
  $service->set_tags( @tags )->wait();

Assign tags to a service.  If a hashref is given, it will be converted to an array
in a sensible manner before passing to daemonproxy, which expects a list.
If the service doesn't exist, daemonproxy will create it.
The state of the object will not reflect changes until you call $proto->flush() or
wait() on the returned command object (like in the 5th example).

=cut

sub tag_list { @{ ($_[0]->_svc || {})->{tags} || []} }
sub tags             { $_ > 1? (shift)->set_tags(@_) : return [ $_[0]->tag_list ] }

sub _tag_hash {
	my $svc= (shift)->_svc;
	$svc->{tags_hash} ||= { map { $_ =~ /^([^=]*)(?:=(.*))?/? ($1 => defined $2? $2 : 1) : () } @{ $svc->{tags} } };
}

sub _tag_hash_to_list {
	my $hash= shift;
	map { $hash->{$_} eq '1'? $_
		: $_.'='.$hash->{$_} } sort keys %$hash;
}

sub set_tags {
	my $self= shift;
	$self->conn->begin_cmd('service.tags', $self->name,
		@_ == 1 && ref $_[0] eq 'ARRAY'? @{$_[0]}
		: @_ == 1 && ref $_[0] eq 'HASH'? _tag_hash_to_list($_[0])
		: @_);
}

=head2 tag_values

  my $foo= $service->tag_values('foo');
  my ($foo, $bar)= $service->tag_values('foo', 'bar');
  my $val_array= $service->tag_values([ 'foo', 'bar' ]);

Treating the tags as a set of name/value pairs, returns one value for each supplied
key name.  Tags of the form "foo=" are considered to have an empty value, but
tags without an '=' like "foo" are considered to be a boolean true, with a value of '1'.

=cut

sub tag_values {
	my $tag_hash= shift->_tag_hash;
	return @_ > 1? @{$tag_hash}{@_}
		: ref $_[0] eq 'ARRAY'? [ @{$tag_hash}{@{$_[0]}} ]
		: $tag_hash->{$_[0]};
}

=head2 set_tag_values

  $service->set_tag_values( foo => 1, bar => 2 );

MERGES the supplied values (or hashref) into the existing tag values for
this service.  The merge is performed by this library, and not by daemonproxy.
To help prevent bugs, this method performs a flush() on the protocol object
first, to make sure we have the latest values.  If you have more than one
controller script this could still fail to work (so don't do that).

=cut

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

=head2 start

Start the named service.  Problems from starting the service will be reported
asynchronously.

=cut

sub start {
	my $self= shift;
	$self->conn->begin_cmd('service.start', $self->name);
}

=head2 signal

  $service->signal( 'SIGTERM' );

Send a signal to a service.  Daemonproxy will send the signal if any only if it
has not reaped the process.  This the the only non-race-condition method to send
signals to a process.

=head2 signal_group

  $service->signal_pgroup( 'SIGHUP' );

Send signal to entire process group (but only if the service leads a process group).

=cut

sub signal {
	my ($self, $signal)= @_;
	$self->conn->begin_cmd('service.signal', $self->name, $signal);
}

sub signal_pgroup {
	my ($self, $signal)= @_;
	$self->conn->begin_cmd('service.signal', $self->name, $signal, 'group');
}

=head2 delete

Ask daemonproxy to delete a service.  You should signal/kill and make sure the
process is reaped before deleting it.  Once deleted, daemonproxy will have no
further knowledge of the process.

=cut

sub delete {
	my $self= shift;
	$self->conn->begin_cmd('service.delete', $self->name);
}

package Daemonproxy::Protocol::FileDescriptor;
use strict;
use warnings;
use overload 'bool' => \&exists;
no warnings 'uninitialized';
use Carp;

=head1 FILE DESCRIPTOR OBJECTS

FileDescriptor objects are a simple wrapper around the C<$proto> object and the
name of a file descriptor.  More than one object can exist for the same fd,
and having the object does not indicate that the fd exists on daemonproxy's side.

FileDescriptor objects support the following attributes/methods:

=head2 (bool)

In boolean context, returns true of the file descriptor exists on daemonproxy's
side, and false otherwise.

=head2 conn

The procotol object this refers to.

=head2 name

The fd name this refers to.

=cut

sub conn { $_[0][0] }
sub name { $_[0][1] }

sub _fd { $_[0][0]->{state}{fds}{$_[0][1]} }

=head2 exists

True if this file descriptor exists on daemonproxy's side.

=head2 type

The type of the file descriptor.  One of 'file', 'pipe', or 'special'.
Undefined if this descriptor doesn't exist.

=head2 is_pipe

True iff this handle is a pipe.

=head2 is_file

True iff this handle is a file.

=head2 is_special

True if this handle is one of the file descriptors daemonproxy started with or
if it is a virtual fd like 'control.event'.

=head2 flags

A coma-separated list of flags associated with this descriptor.  See
daemonproxy manual.

=head2 description

A string describing this fd.  See daemonproxy manual.

=cut

sub exists {
	defined shift->_fd;
}

sub type        { ($_[0]->_fd || {})->{type} }

sub is_pipe    { ($_[0]->_fd || {})->{type} eq 'pipe' }
sub is_file    { ($_[0]->_fd || {})->{type} eq 'file' }
sub is_special { ($_[0]->_fd || {})->{type} eq 'special' }

sub flags       { ($_[0]->_fd || {})->{flags} }
sub description { ($_[0]->_fd || {})->{descrip} }

=head2 open_file

  $proto->fd('zero')->open_file('/dev/zero');
  $proto->fd('zero')->open_file('/dev/zero', 'read,nonblock');
  $proto->fd('zero')->open_file('/dev/zero', ['read', 'nonblock']);
  $proto->fd('zero')->open_file('/dev/zero', { read => 1, nonblock => 1 });

Open the specified path with the specified flags.  Flags can be nothing,
a comma-delimited string, an array, or a set (hashref).
daemonproxy performs the 'open' call, so *it* must be have sufficient
permissions, rather than the current script.

Returns a command object which you can call L<wait> on.

=cut

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

=head2 open_pipe_to

  $proto->fd('pipe.w')->open_pipe_to('pipe.r');

Create a pipe from the first FD name to the second.

Returns a command object which you can call L<wait> on.

=head2 open_pipe_from

  $proto->fd('pipe.r')->open_pipe_from('pipe.w');

Same as above, but name order reversed.

=cut

sub open_pipe_to {
	my ($self, $dest_name)= @_;
	defined $dest_name
		or croak "Require pipe read-end name";
	$dest_name= $dest_name->name
		if blessed($dest_name) and $dest_name->can('name');
	$self->conn->begin_cmd('fd.pipe', $dest_name, $self->name);
}

sub open_pipe_from {
	my ($self, $src_name)= @_;
	defined $src_name
		or croak "Require pipe write-end name";
	$src_name= $src_name->name
		if blessed($src_name) and $src_name->can('name');
	$self->conn->begin_cmd('fd.pipe', $self->name, $src_name);
}

=head2 delete

Tell daemonproxy to close and forget a file descriptor.

=cut

sub delete {
	my $self= shift;
	$self->conn->begin_cmd('fd.delete', $self->name);
}

=head1 COMMAND OBJECTS

Most methods in this library which generate daemonproxy commands will
return a Command object, if called in non-void context.  The command
object can be used for synchronization to ensure that daemonproxy has
received all the previous commands and we have received all of the
events from them.

=head2 wait

This method calls L<pump_events> on the main connection object until
this command is complete.  This method should be forward-compatible
with future support for AnyEvent, though that support is not built yet.

=cut

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