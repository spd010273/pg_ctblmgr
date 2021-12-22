#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>

#include "../src/lib/shm.h"

#define TEST_SIZE 2048

int main( void );
static void print_bin( uint64_t );

const char * bits[16] = {
    [ 0] = "0000", [ 1] = "0001", [ 2] = "0010", [ 3] = "0011",
    [ 4] = "0100", [ 5] = "0101", [ 6] = "0110", [ 7] = "0111",
    [ 8] = "1000", [ 9] = "1001", [10] = "1010", [11] = "1011",
    [12] = "1100", [13] = "1101", [14] = "1110", [15] = "1111",
};

static void fill_block( void *, size_t );
static void check_block( void *, size_t );
static void child_routine( __ref );
static void print_block( void *, size_t );
static void random_fill( void *, size_t ) __attribute__((unused));
int main( void )
{
    pid_t child = 0;
    __ref data = {0};
    void * mapped_addr = NULL;

    // Phase I - initialize, allocate a segment, blank and write the whole page
    // Phase II - fork(), have the child verify the page and blank the page
    // Phase III - parent and child test the __ref struct to verify that differential pointers work


    // Begin - Phase I
    fprintf( stdout, "PHASE 1 TEST\n" );
    shm_init();
    mapped_addr = new_segment( TEST_SIZE );
    fprintf( stdout, "got mapping of %p\n", mapped_addr );

    //print_block( mapped_addr, ( size_t ) TEST_SIZE );
    fill_block( mapped_addr, ( size_t ) TEST_SIZE );
    check_block( mapped_addr, ( size_t ) TEST_SIZE );
    //print_block( mapped_addr, ( size_t ) TEST_SIZE );

    // Begin - Phase II
    fprintf( stdout, "PHASE 2 TEST\n" );
    fprintf( stdout, "Getting reference to pass to child\n" );
    data = get_ref( mapped_addr );
    fprintf( stdout, "Generated ref %lu, %zu\n", ( uint64_t ) data._segment, data._offset );
    child = fork();

    if( child == 0 ) // child
    {
        child_routine( data );
        exit( 1 );
    }

    wait( NULL );
    unmap_all();
    return 0;
}

static void print_byte( uint8_t data )
{
    fprintf( stdout, "%s%s", bits[data >> 4], bits[ data & 0x0F ] );
    return;
}

static void print_bin( uint64_t data )
{
    print_byte( ( uint8_t ) ( ( data >> 56 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 48 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 40 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 32 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 24 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 16 ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data >> 8  ) & 0xFF ) );
    print_byte( ( uint8_t ) ( ( data       ) & 0xFF ) );
    fprintf( stdout, "\n" );
    return;
}

// XXX overrunning bounds of memory array by a large-ish amound
static void child_routine( __ref data )
{
    ctrl_header * orig_mapped = NULL;
    ctrl_header * header      = NULL;
    void *        mapping     = NULL;

    orig_mapped = get_control_header();
    shm_child_init();
    header = get_control_header();

    if( header != orig_mapped )
    {
        fprintf(
            stderr,
            "Child: control mapped %p, original (parent): %p\n",
            ( void * ) header,
            ( void * ) orig_mapped
        );
    }

    // attempt the auto pointer deref
    fprintf(  stdout, "Attempting automap\n" );
    mapping = get_ptr( data );
    fprintf( stdout, "Got mapping for foreign segment %p\n", mapping );
    check_block( mapping, ( size_t ) TEST_SIZE );
    fprintf( stdout, "Child mapping good, performing write test\n" );
    fill_block( mapping, ( size_t ) TEST_SIZE );
    fprintf( stdout, "Child write test done, performing readback prior to exiting\n" );
    check_block( mapping, ( size_t ) TEST_SIZE );
    print_block( mapping, ( size_t ) TEST_SIZE );
    zero_segment( data._segment );
    fprintf( stdout, "===========================================================\n" );
    random_fill(mapping, ( size_t ) TEST_SIZE );
    print_block( mapping, ( size_t ) TEST_SIZE );
    unmap_all();
    return;
}

static void random_fill( void * mapped_addr, size_t size )
{
    uint64_t i = 0;
    uint64_t * ptr = NULL;
    uint32_t a = 0;
    uint32_t b = 0;
    uint64_t data = 0;

    ptr = ( uint64_t * ) mapped_addr;
    for( i = 0; i < ( size / sizeof( uint64_t ) ); i++ )
    {
        a = random();
        b = random();
        data = ( (uint64_t) a << 32 ) | (uint64_t) b;
        *(ptr + i) = data;
    }

    return;
}

static void fill_block( void * mapped_addr, size_t size )
{
    uint64_t * ptr = NULL;
    uint64_t data64 = 0;
    uint64_t data64_1 = 0;
    uint64_t data64_2 = 0;
    uint64_t i = 0;
    uint64_t bytes_out = 0;

    if( size % 8 != 0 )
    {
        fprintf( stderr, "size needs to be a multiple of 8\n" );
        return;
    }

    ptr = ( uint64_t * ) mapped_addr;
    i = 0;
    for( data64 = 1; data64 != 0; data64 <<= 1 )
    {
        *(ptr + i) = data64;
        i++;
        bytes_out += sizeof( uint64_t );
        if( bytes_out >= size )
            return;
    }

    for(
            data64_1 = 1, data64_2 = 0x8000000000000000;
            data64_1 != 0;
            data64_1 <<= 1, data64_2 >>= 1
       )
    {
        data64 = data64_1 | data64_2;
        *(ptr + i) = data64;

        i++;
        bytes_out += sizeof( uint64_t );
        if( bytes_out >= size )
            return;
    }

    return;
}

static void print_block( void * ptr, size_t size )
{
    uint64_t i = 0;
    uint64_t * d = NULL;

    d = ( uint64_t * ) ptr;
    fprintf( stderr, "Contents of memory starting at %p\n", ptr );

    for( i = 0; i < ( size / sizeof( uint64_t ) ); i++ )
    {
        fprintf(
            stderr,
            "%p (%p + %04lu): ",
            d + i,
            d,
            i
        );

        print_bin( *( d + i ) );
    }

    return;
}

static void check_block( void * mapped_addr, size_t size )
{
    uint64_t * ptr      = NULL;
    uint64_t   data64   = 0;
    uint64_t   data64_1 = 0;
    uint64_t   data64_2 = 0;
    uint32_t   i        = 0;

    if( ( size % 8 ) != 0 )
    {
        fprintf( stderr, "Size needs to be a multiple of 8\n" );
        return;
    }

    ptr = ( uint64_t * ) mapped_addr;

    for( data64 = 1; data64 != 0; data64 <<= 1 )
    {
        if( *(ptr + i ) != data64 )
        {
            fprintf( stderr, "Failed pattern " );
            print_bin( data64 );
            return;
        }

        i++;
    }

    for(
            data64_1 = 1, data64_2 = 0x8000000000000000;
            data64_1 != 0;
            data64_1 <<= 1, data64_2 >>= 1
       )
    {
        data64 = data64_1 | data64_2;

        if( *(ptr + i) != data64 )
        {
            fprintf( stderr, "Failed pattern " );
            print_bin( data64 );
            return;
        }

        i++;
    }

    return;
}

static void check_ref_logic( void * mapped_address, size_t size )
{
    uint64_t * ptr = NULL;
    uint64_t i = 0;
    uint64_t * d = NULL;
    uint64_t * t = NULL;

    __ref test_ref = {0};
    ptr = ( uint64_t * ) mapped_address;

    for( i = 0; i < size; i++ )
    {
        d = ptr + i;
        test_ref = get_ref( ( void * ) d );

        if( test_ref._segment == SEGMENT_HANDLE_INVALID )
        {
            fprintf( stderr, "FAILED: Could not generate a reference for address %p\n", d );
            return;
        }

        t = get_ptr( test_ref );

        if( t != d )
        {
            fprintf(
                stderr,
                "FAILED: dereferenced __ref (%lu,%zu) = %p != %p\n",
                ( uint64_t ) test_ref._segment,
                test_ref._offset,
                ( void * ) t,
                ( void * ) d
            );
            return;
        }
    }

    return;
}

