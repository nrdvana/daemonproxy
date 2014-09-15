#! /usr/bin/env perl

use strict;
use warnings;
use Test::More;
use FindBin;
use Try::Tiny;
use lib "$FindBin::Bin/lib";

use_ok( 'TestDpProto' ) or BAIL_OUT;
use_ok( 'Daemonproxy::Protocol' ) or BAIL_OUT;
ok( try { mock_dp()->client }, 'create mock_dp and client' ) or BAIL_OUT;

done_testing;
