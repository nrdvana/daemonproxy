#! /usr/bin/env perl
use FindBin;
use lib "$FindBin::Bin/lib";
use TestDpProto;

my $dp= mock_dp;
my $p= $dp->client;

subtest fd_object => sub {
	isa_ok( my $fd_foo= $p->fd('foo'), 'Daemonproxy::Protocol::FileDescriptor' );
	is( $fd_foo->name, 'foo', 'make was captured' );
	ok( !$fd_foo->exists, 'foo does not exist yet' );
	ok( !$fd_foo, 'foo tests false' );
	is( $fd_foo->type, undef );
	ok( !$fd_foo->is_pipe, 'is_pipe' );
	ok( !$fd_foo->is_file, 'is_file' );
	ok( !$fd_foo->is_special, 'is_special' );
	
	$dp->send_event('fd.state', 'foo', 'pipe', 'to', 'bar');
	$p->pump_events;
	
	ok( $fd_foo->exists, 'foo exists' );
	ok( $fd_foo, 'foo tests true' );
	is( $fd_foo->type, 'pipe' );
	ok( $fd_foo->is_pipe, 'is_pipe' );
	ok( !$fd_foo->is_file, 'is_file' );
	ok( !$fd_foo->is_special, 'is_special' );
	is( $fd_foo->flags, 'to' );
	is( $fd_foo->description, 'bar' );
};

done_testing;
