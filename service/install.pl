#!/usr/bin/perl

=pod
/*------------------------------------------------------------------------
 *
 * install.pl
 *     Basic systemctl installer for pg_ctblmgr
 *
 * Copyright (c) 2018, Nead Werx, Inc.
 * Copyright (c) 2021, MerchLogix Inc.
 *
 * IDENTIFICATION
 *        install.pl
 *
 *------------------------------------------------------------------------
 */
=cut

use strict;
use warnings;

use Readonly;
use Carp;
use Fatal qw( open );
use Cwd;
use DBI;
use English qw( -no_match_vars );
use Params::Validate qw( :all );
use Getopt::Std;
use File::Copy;
use File::Path qw( make_path );
use IO::Interactive qw( is_interactive );

Readonly my $LOG_DIR             => '/var/log/pg_ctblmgr/';
Readonly my $LOG_FILE            => 'pg_ctblmgr.log';
Readonly my $START_FILE_NAME     => 'pg_ctblmgr-startup.sh';
Readonly my $STOP_FILE_NAME      => 'pg_ctblmgr-shutdown.sh';
Readonly my $RELOAD_FILE_NAME    => 'pg_ctblmgr-reload.sh';
Readonly my $SH_TARGET_DIR       => '/usr/bin/';
Readonly my $SYSTEMD_SERVICE_DIR => '/usr/lib/systemd/system/';
Readonly my $LOGROTATE_DEST      => '/etc/logrotate.d/pg_ctblmgr';
Readonly my $GIT_INIT_COMMAND    => 'git submodule update --init --recursive';

Readonly my $START_SH => <<BASH;
#!/bin/bash
#    This script will start the pg_ctblmgr daemons
trap "" SIGHUP
__INSTALL_DIR__/pg_ctblmgr -U __USERNAME__ -d __DBNAME__ -h __HOSTNAME__ -p __PORT__
BASH

Readonly my $STOP_SH => <<BASH;
#!/bin/bash
#    This script will stop the pg_ctblmgr daemons
pkill -0 pg_ctblmgr
exit 0
BASH

Readonly my $RELOAD_SH => <<BASH;
#!/bin/bash
#   This script will issue a SIGHUP to pg_ctblmgr
pkill -1 pg_ctblmgr
BASH

my $hostname;
my $username;
my $port;
my $dbname;

sub get_user_input($)
{
    my( $message ) = validate_pos(
        @_,
        { type => SCALAR }
    );

    print "$message\n";
    my $response = <STDIN>;
    chomp( $response );

    return $response;
}

sub build_repo()
{
    # It's expected we're in the pg_ctblmgr root
    system( $GIT_INIT_COMMAND );
    #TODO:
    #  - Check for lib prerequisites

    system( 'make clean' );
    system( 'make' );
    system( 'make install' );

    return;
}

sub test_connection()
{
    my $connection_string = "dbi:Pg:dbname=${dbname};host=${hostname};port=${port}";
    my $handle = DBI->connect(
        $connection_string,
        $username,
        undef
    );

    unless( defined $handle )
    {
        return 0;
    }

    if( $handle->ping() > 0 )
    {
        $handle->disconnect();
        return 1;
    }

    return 0;
}

sub get_username()
{
    if( is_interactive() )
    {
        $username = get_user_input( 'Please enter a username:' );
    }
    else
    {
        $username = 'postgres';
    }

    return;
}

sub get_hostname()
{
    if( is_interactive() )
    {
        $hostname = get_user_input( 'Please enter a hostname:' );
    }
    else
    {
        $hostname = 'localhost';
    }

    return;
}

sub get_port()
{
    if( is_interactive() )
    {
        $port = get_user_input( 'Please enter a port:' );
    }
    else
    {
        $port = 5432;
    }

    return;
}

sub get_dbname()
{
    if( is_interactive() )
    {
        $dbname = get_user_input( 'Please enter a database name:' );
    }
    else
    {
        carp 'No database name provided :|';
        $dbname = 'postgres';
    }

    return;
}


## Main Program
if( $EFFECTIVE_USER_ID != 0 )
{
    croak( 'Must be root' );
}

unless( -e $LOG_DIR )
{
    unless( make_path( $LOG_DIR ) )
    {
        croak "Failed to create logging directory $LOG_DIR";
    }
}

my $dir = getcwd();

unless( -e "$dir/../pg_ctblmgr" )
{
    croak 'Could not locate pg_ctblmgr daemon - is it built?';
}

if( $dir =~ /\/service/ )
{
    chdir( '../' );
}

build_repo();

my $install_dir = getcwd();
chdir( $dir );

our( $opt_d, $opt_U, $opt_p, $opt_h, $opt_E, $opt_W );

unless( getopts( 'd:U:p:h:' ) )
{
    get_dbname();
    get_username();
    get_hostname();
    get_port();
}

GET_ARGS:
if( defined $opt_d and length( $opt_d ) > 0 )
{
    $dbname = $opt_d;
}
else
{
    get_dbname();
}

if( defined $opt_U and length( $opt_U ) > 0 )
{
    $username = $opt_U;
}
else
{
    get_username();
}

if( defined $opt_h and length( $opt_h ) > 0 )
{
    $hostname = $opt_h;
}
else
{
    get_hostname();
}

if( defined $opt_p and $opt_p =~ /^\d+$/ )
{
    $port = $opt_p;
}
else
{
    get_port();
}


unless( test_connection() )
{
    carp 'Connection parameters do not work';
    goto GET_ARGS;
}

my $start_shell_script = $START_SH;
$start_shell_script    =~ s/__INSTALL_DIR__/$install_dir/g;
$start_shell_script    =~ s/__USERNAME__/$username/g;
$start_shell_script    =~ s/__DBNAME__/$dbname/g;
$start_shell_script    =~ s/__HOSTNAME__/$hostname/g;
$start_shell_script    =~ s/__PORT__/$port/g;

my $stop_shell_script  = $STOP_SH;
$stop_shell_script     =~ s/__INSTALL_DIR__/$install_dir/g;
$stop_shell_script     =~ s/__USERNAME__/$username/g;
$stop_shell_script     =~ s/__DBNAME__/$dbname/g;
$stop_shell_script     =~ s/__HOSTNAME__/$hostname/g;
$stop_shell_script     =~ s/__PORT__/$port/g;

my $reload_shell_script = $RELOAD_SH;
$reload_shell_script    =~ s/__INSTALL_DIR__/$install_dir/g;
$reload_shell_script    =~ s/__USERNAME__/$username/g;
$reload_shell_script    =~ s/__DBNAME__/$dbname/g;
$reload_shell_script    =~ s/__HOSTNAME__/$hostname/g;
$reload_shell_script    =~ s/__PORT__/$port/g;

unless( open( STARTFILE, ">${SH_TARGET_DIR}${START_FILE_NAME}" ) )
{
    croak "Failed to write to ${SH_TARGET_DIR} for systemd startup script";
}

print STARTFILE $start_shell_script;
close STARTFILE;
chmod "0755", "${SH_TARGET_DIR}${START_FILE_NAME}";

unless( open( STOPFILE, ">${SH_TARGET_DIR}${STOP_FILE_NAME}" ) )
{
    croak "Failed to write to ${SH_TARGET_DIR} for systemd shutdown script";
}

print STOPFILE $stop_shell_script;
close STOPFILE;
chmod "0755", "${SH_TARGET_DIR}${STOP_FILE_NAME}";

unless( open( RELOADFILE, ">${SH_TARGET_DIR}${RELOAD_FILE_NAME}" ) )
{
    croak "Failed to write to ${SH_TARGET_DIR} for systemd reload script";
}

print RELOADFILE $reload_shell_script;
close RELOADFILE;
chmod "0755", "${SH_TARGET_DIR}${RELOAD_FILE_NAME}";

if(
        -e "${SH_TARGET_DIR}${START_FILE_NAME}"
    and -e "${SH_TARGET_DIR}${STOP_FILE_NAME}"
    and -e "${SH_TARGET_DIR}${RELOAD_FILE_NAME}"
  )
{
    print "Copied start/stop scripts to ${SH_TARGET_DIR}\n";
}
else
{
    croak "Failed to setup start/stop scripts in ${SH_TARGET_DIR}\n";
}

unless( -e $SYSTEMD_SERVICE_DIR )
{
    croak 'Is systemd installed??';
}

unless( copy( "${install_dir}/service/templates/pg_ctblmgr.service", $SYSTEMD_SERVICE_DIR ) )
{
    print "source dir is ${install_dir}/service/templates/pg_ctblmgr.service\n";
    croak "Failed to copy file to $SYSTEMD_SERVICE_DIR: $OS_ERROR";
}

my $result = system( "systemd-analyze verify ${SYSTEMD_SERVICE_DIR}/pg_ctblmgr.service" );

if( $result )
{
    croak "Service installation failed";
}

unless( copy( "${install_dir}/service/templates/pg_ctblmgr.logrotate", $LOGROTATE_DEST ) )
{
    croak "Failed to setup logrotation: $OS_ERROR";
}

system( 'systemctl daemon-reload' );
system( 'systemctl enable pg_ctblmgr.service' );
exit 0;
