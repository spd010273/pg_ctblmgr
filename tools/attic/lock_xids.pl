#!/usr/bin/perl

use strict;
use warnings;
use utf8;

use Params::Validate qw( :all );
use Readonly;
use English qw( -no_match_vars );

use Getopt::Std;
use IPC::SysV;
use Data::Dumper;
use Text::Table;
use DBI;

use FindBin;
use lib "$FindBin::Bin/../../service/lib";

use Util;
use Shm;
Readonly my $XID_KEY => 17783314;


my $XID_MAP = [];
unless( get_or_create_shm( $XID_KEY ) )
{
    warn( "Could not tie XID_MAP - is $EXTENSION_NAME running?\n" );
    return undef;
}

do_lock( $XID_KEY, $WRITE_LOCK );
$XID_MAP = readmem( $XID_KEY );
print Dumper( $XID_MAP );
foreach my $xid( @$XID_MAP )
{
    push( @{$xid->{in_use}}, $PROCESS_ID );
}
print Dumper( $XID_MAP );
writemem( $XID_KEY, $XID_MAP );
do_lock( $XID_KEY, $WRITE_UNLOCK );
