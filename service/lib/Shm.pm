package Shm;

use JSON::XS;
use Readonly;
use IPC::SysV qw( :all );
use Perl6::Export::Attrs;
use Params::Validate qw( :all );
use strict;
use warnings;

Readonly::Scalar our $WRITE_LOCK   :Export( :MANDATORY ) => 'WL'; 
Readonly::Scalar our $WRITE_UNLOCK :Export( :MANDATORY ) => 'WUL';
Readonly::Scalar our $READ_LOCK    :Export( :MANDATORY ) => 'RL';
Readonly::Scalar our $READ_UNLOCK  :Export( :MANDATORY ) => 'RUL';
Readonly::Scalar our $READ_TO_WRITE :Export( :MANDATORY ) => 'R2W'; # upgrade a read exclusive
Readonly::Scalar our $WRITE_TO_READ :Export( :MANDATORY ) => 'W2R'; # downgrade a write exclusive
Readonly::Scalar our $READ_NOWAIT :Export( :MANDATORY ) => 'RNW';
Readonly my $SHM_CREATE_FLAGS => IPC_EXCL | IPC_CREAT;
Readonly my $SEM_PERM_FLAGS   => ( S_IRUSR | S_IWUSR );
Readonly my $SHM_PERM_FLAGS   => ( S_IRUSR | S_IWUSR );
Readonly my $SEMOP_ARGS       => {
    $WRITE_LOCK => [
        0, 0, 0,    # Wait for writers
        1, 0, 0,    # Wait for readers
        0, 1, SEM_UNDO # Assert write
    ],
    $WRITE_UNLOCK => [
        0, -1, IPC_NOWAIT # deassert write
    ],
    $READ_LOCK => [
        0, 0, 0,    # Wait for writers
        1, 1, SEM_UNDO # Assert read
    ],
    $READ_UNLOCK => [
        1, -1, IPC_NOWAIT # Deassert read
    ],
    $READ_TO_WRITE => [
        0, 0, 0,    # Wait for writers
        1, -1, 0,   # Deassert read
        1, 0, 0,    # Wait for other readers
        0, 1, SEM_UNDO # assert write
    ],
    $WRITE_TO_READ => [
        0, -1, IPC_NOWAIT,              # Deassert write
        1, 1, ( IPC_NOWAIT | SEM_UNDO ) # Assert read
    ],
    $READ_NOWAIT => [
        0, 0, IPC_NOWAIT,
        1, 1, SEM_UNDO
    ],
};

Readonly my $PACKMOD => do { my $foobar = eval { pack( "L!", 0 ); }; $@ ? '' : '!' };
Readonly my $DEFAULT_ALLOCSIZE => 65536;
## structure is {SEM/SHM ID}->{ sem => semget key, shm => shmget key }
my $ACTIVE_KEYS = {};
my $_PARENT_PID;
# Initializes a new shm segment and semaphore set for the segment

sub do_shm_cleanup() :Export( :MANDATORY )
{
    # iterate through active_keys and prune them
    return unless( $_PARENT_PID && $_PARENT_PID == $$ );
    foreach my $id( keys %$ACTIVE_KEYS )
    {
        shmctl( $ACTIVE_KEYS->{$id}->{shm}, IPC_RMID, 0 );
        semctl( $ACTIVE_KEYS->{$id}->{sem}, 0, IPC_RMID, 0 );
    }

    undef( $ACTIVE_KEYS );
    return;
}

sub do_cleanup_key($) :Export( :MANDATORY )
{
    my( $id ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    # attempt to ipcrm id->key, dont fail
    my $shm;
    my $sem;
    my $SEM_ID = semget( $id, 2, 0 );
    my $SHM_ID = shmget( $id, 0, 0 );
    $shm = shmctl( $SHM_ID, IPC_RMID, 0 ) if( $SHM_ID );
    $sem = semctl( $SEM_ID, 0, IPC_RMID, 0 ) if( $SEM_ID );
    
    return;
}

sub do_lock($$) :Export( :MANDATORY )
{
    my( $id, $mode ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => SCALAR },
    );

    my $area_info = { };
    if( !defined( $ACTIVE_KEYS->{$id} ) )
    {
        $area_info = &get_or_create_shm( $id );
        return 0 unless( $area_info );
    }
    else
    {
        $area_info = $ACTIVE_KEYS->{$id};
    }

    my $lockdata = pack( "s$PACKMOD*", @{$SEMOP_ARGS->{$mode}} );
    return semop( $area_info->{sem}, $lockdata );
}

sub stat_shm($)
{
    my( $id ) = validate_pos(
        @_,
        { type => SCALAR }
    );

    my $shm_stat = '';
    if( defined( $ACTIVE_KEYS->{$id} ) )
    {
        unless( shmctl( $ACTIVE_KEYS->{$id}, IPC_STAT, $shm_stat ) )
        {
            warn "shmstat failed\n";
            return 0;
        }
    }
    else
    {
        my $shmkey = shmget( $id, 0, 0 );
        return 0 if( !defined( $shmkey ) );

        return 0 unless( shmctl( $shmkey, IPC_STAT, $shm_stat ) );
    }

    my @data = unpack( 'i*', $shm_stat );
    return $data[12];
}

sub get_or_create_shm($) :Export( :MANDATORY )
{
    my( $id ) = validate_pos(
        @_,
        { type => SCALAR }
    );

    return $ACTIVE_KEYS->{$id} if( defined( $ACTIVE_KEYS->{$id} ) );
    my $sem_key;
    my $shm_key;
    my $size = stat_shm( $id );
    if( defined( $size ) && $size > 0 )
    {
        # Segment exists, let's attach
        $sem_key = semget( $id, 2, 0 );
        $shm_key = shmget( $id, 0, 0 );
    }
    else
    {
        return undef if( !defined( $_PARENT_PID ) || $$ != $_PARENT_PID );
        $sem_key = semget( $id, 2, $SHM_CREATE_FLAGS | $SEM_PERM_FLAGS );
        $shm_key = shmget( $id, $DEFAULT_ALLOCSIZE, $SHM_CREATE_FLAGS | $SHM_PERM_FLAGS );
        $size    = $DEFAULT_ALLOCSIZE;
    }

    $ACTIVE_KEYS->{$id} = { sem => $sem_key, shm => $shm_key, shm_size => $size };
    return $ACTIVE_KEYS->{$id};
}

sub readmem($) :Export( :MANDATORY )
{
    my( $id ) = validate_pos(
        @_,
        { type => SCALAR }
    );

    my $shmdata = get_or_create_shm( $id );

    if( $shmdata )
    {
        my $data = '';
        unless( shmread( $ACTIVE_KEYS->{$id}->{shm}, $data, 0, $ACTIVE_KEYS->{$id}->{shm_size} ) )
        {
            return undef;
        }
        $data =~ s/\x{0}.*$// if( $data );
        my $json = decode_json( $data ) if( $data );
        return $json;

    }

    return undef;
}

sub writemem($$) :Export( :MANDATORY )
{
    my( $id, $data ) = validate_pos(
        @_,
        { type => SCALAR },
        { type => HASHREF | ARRAYREF | SCALAR | SCALARREF },
    );

    my $json_string = encode_json( $data );
    my $shmdata = get_or_create_shm( $id );

    if( $shmdata )
    {
        if( shmwrite( $ACTIVE_KEYS->{$id}->{shm}, $json_string, 0, $ACTIVE_KEYS->{$id}->{shm_size} ) )
        {
            return 1;
        }
    }

    return 0;
}

sub shminit($) :Export( :MANDATORY )
{
    my( $pid ) = validate_pos(
        @_,
        { type => SCALAR },
    );

    if( defined( $_PARENT_PID ) && $_PARENT_PID )
    {
        return 0;
    }

    $_PARENT_PID = $pid;
    return $_PARENT_PID;
}

1;
