#! /usr/bin/env perl
use FindBin;
use lib "$FindBin::Bin/lib";
use TestDpProto;

my $dp= mock_dp;
my $p= $dp->client;

subtest service_object => sub {
	isa_ok( my $svc_foo= $p->service('foo'), 'Daemonproxy::Protocol::Service' );
	is( $svc_foo->name, 'foo', 'nonexistent service name' );
	ok( !$svc_foo->exists, 'foo does not exist yet' );
	ok( !$svc_foo, 'foo tests false' );

	$dp->send_event("service.fds", "foo");
	$p->pump_events;

	ok( $svc_foo->exists, 'foo exists' );
	ok( $svc_foo, 'foo tests true' );
	is_deeply( $svc_foo->file_descriptors, [], 'empty fd list' );
};

subtest args_and_fds => sub {
	my $svc_foo= $p->service('foo');
	$dp->send_event("service.args", "foo", "/bin/false");
	$dp->send_event("service.fds", "foo", "null", "log", "log");
	$p->pump_events;

	is_deeply( $svc_foo->arguments, [ '/bin/false' ], 'one arg' );
	is_deeply( $svc_foo->file_descriptors, [ 'null', 'log', 'log' ], '3 fds' );
	
	my $svc_bar= $p->service('bar');
	$svc_bar->set_arguments('a', 'b');
	is_deeply( scalar $dp->next_cmd, [ 'service.args', 'bar', 'a', 'b' ], 'set args' );
	is_deeply( $svc_bar->arguments, [], 'change doesn\'t take effect til flush'  );
};

subtest tags => sub {
	my $svc_foo= $p->service('foo');
	$dp->send_event("service.tags", "foo", "from_json", "thing=", "filename=./foo/rc.main");
	$p->pump_events;

	is_deeply( $svc_foo->tags, [ 'from_json', 'thing=', 'filename=./foo/rc.main' ], 'two tags' );
	is( $svc_foo->tag_values('filename'), './foo/rc.main', 'tag value of filename' );
	is( $svc_foo->tag_values('from_json'), 1, 'boolean tag value' );
	is( $svc_foo->tag_values('thing'), '', 'empty tag value' );
	is( $svc_foo->tag_values('invalid'), undef, 'missing tag value' );
	is_deeply( [ $svc_foo->tag_values('thing', 'invalid', 'from_json') ], [ '', undef, 1 ], 'multiple values' );
};

done_testing;
