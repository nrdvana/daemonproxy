package Test::MockDaemonproxy;
use strict;
use warnings;
use IO::Handle;
use Log::Any '$log';
use Daemonproxy::Protocol;

sub new {
	my $class= shift;
	my %args= @_ == 1 && ref $_[0] eq 'HASH'? %{ $_[0] } : @_;
	pipe($args{event_rd}, $args{event_wr});
	pipe($args{cmd_rd}, $args{cmd_wr});
	bless \%args, $class;
}

sub client {
	my $self= shift;
	return $self->{client} ||= Daemonproxy::Protocol->new(
		rd_handle => $self->{event_rd},
		wr_handle => $self->{event_wr}
	);
}

sub next_cmd {
	my $self= shift;
	$self->{cmd_rd}->blocking(0);
	my $line= $self->{cmd_rd}->getline;
	return unless defined $line;
	chomp $line;
	return [ split /\t/, $line ];
}

sub send_event {
	my ($self, @args)= @_;
	my $msg= join("\t", @args);
	$log->tracef("mock event: %s", $msg) if $log->is_trace;
	$self->{event_wr}->print($msg."\n");
}

1;
