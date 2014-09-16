package TestDpProto;
use strict;
use warnings;
use Carp;
use FindBin;
use Try::Tiny;
use Test::More;
use Log::Any '$log';
use Log::Any::Adapter 'TAP';
use lib "$FindBin::Bin/../../../../t/lib";
require Exporter;
our @EXPORT= qw( dp client client2 mock_dp );

sub import {
	my $caller= caller;
	strict->import;
	warnings->import;
	eval "package $caller; use Try::Tiny; use Test::More; use Log::Any '\$log';";
	goto &Exporter::import;
}

our $mock_dp;
sub mock_dp {
	$mock_dp ||= do {
		require Test::MockDaemonproxy;
		Test::MockDaemonproxy->new;
	};
}

our $dp;
sub dp {
	$dp ||= do {
		require Test::Daemonproxy;
		Test::Daemonproxy->new();
	};
}

our $client1;
sub dp_client1 {
	$client1 ||= do {
		require Daemonproxy::Protocol;
		Daemonproxy::Protocol->new(
			rd_handle => dp->dp_stdout,
			wr_handle => dp->dp_stdin
		);
	};
}
*dp_client= *dp_client1;

our $client2;
sub dp_client2 {
	$client2 ||= do {
		require Socket;
		
		# The only way to connect an additional client to daemonproxy is
		# to either spawn a new client process, or connect via socket.
		# We opt for the socket route here.  The alternative is to define
		# and start a new service which relays its handles to pipes that
		# we created when spawning daemonproxy.
		-d "$FindBin::Bin/tmp" or mkdir("$FindBin::Bin/tmp") or croak "can't create ./tmp dir";
		my $sockpath= "$FindBin::Bin/tmp/daemonproxy-client2.sock";
		unlink $sockpath;
		
		# Create a socket, connect to it, and then remove it
		
		client->send("socket.create", "-", $sockpath);
		for (1..100) { last if -S $sockpath; sleep 0.1; }
		socket(my $sock, Socket::AF_UNIX(), Socket::SOCK_STREAM(), 0)
			or die "Can't create socket: $!";
		connect($sock, Socket::sockaddr_un($sockpath))
			or croak "Can't connect to $sockpath";
		client->send("socket.delete");
		
		# Return protocol client on this new socket
		DaemonProxy::Protocol->new(
			rd_handle => $sock,
			wr_handle => $sock
		);
	}
}

1;
