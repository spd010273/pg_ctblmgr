#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/lib/changeset.h"

/* Here we define the test changeset string, their WAL level, and the expected
 * output structure */
#define NUM_TESTS 5

const char * tests[NUM_TESTS] = {
    "{\"d\":\"I\",\"x\":\"4408\",\"s\":\"public\",\"t\":\"tb_a\",\"key\":{\"foo\":1}}",
    "{\"type\":\"INSERT\",\"xid\":\"4408\",\"timestamp\":\"2020-01-15 15:29:58.892742-05\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":1},\"data\":{\"new\":{\"foo\":4,\"bar\":5,\"baz\":6}}}",
    "{\"type\":\"INSERT\",\"xid\":\"4408\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":1}}",
    "{\"type\":\"UPDATE\",\"xid\":\"4408\",\"timestamp\":\"2020-01-15 15:29:58.892742-05\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":2},\"data\":{\"new\":{\"foo\":5,\"bar\":6,\"baz\":7},\"old\":{\"foo\":4,\"bar\":5,\"baz\":6}}}",
    "{\"data\":{\"new\":{\"foo\":5,\"bar\":6,\"baz\":7},\"old\":{\"foo\":4,\"bar\":5,\"baz\":6}},\"type\":\"UPDATE\",\"xid\":\"4409\",\"timestamp\":\"2020-01-15 15:29:58.892742-05\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":2}}"
};

const pg_ctblmgr_wal_level wal_levels[NUM_TESTS] = {
    PGC_WAL_MINIMAL,
    PGC_WAL_FULL,
    PGC_WAL_REDUCED,
    PGC_WAL_FULL,
    PGC_WAL_FULL
};

const uint8_t expect_keys_len[NUM_TESTS] = {
    1,1,1,1,1
};
const char * expect_keys[NUM_TESTS] = {
    "foo",
    "foo",
    "foo",
    "foo",
    "foo"
};
const uint8_t expect_vals_len[NUM_TESTS] = {
    1,1,1,1,1
};
const char * expect_vals[NUM_TESTS] = {
    "1",
    "1",
    "1",
    "2",
    "2"
};

const uint8_t expect_columns_len[NUM_TESTS] = {
    0, 3, 0, 3, 3
};

const char * expect_columns[NUM_TESTS][3] = {
    { NULL },
    { "foo", "bar", "baz" },
    { NULL },
    { "foo", "bar", "baz" },
    { "foo", "bar", "baz" }
};

const uint8_t expect_new_len[NUM_TESTS] = {
    0, 3, 0, 3, 3
};

const char * expect_new[NUM_TESTS][3] = {
    { NULL },
    { "4", "5", "6" },
    { NULL },
    { "5", "6", "7" },
    { "5", "6", "7" }
};

const uint8_t expect_old_len[NUM_TESTS] = {
    0, 0, 0, 3, 3
};
const char * expect_old[NUM_TESTS][3] = {
    { NULL },
    { NULL },
    { NULL },
    { "4", "5", "6" },
    { "4", "5", "6" }
};

const char * expects_schema[NUM_TESTS] = {
    "public",
    "public",
    "public",
    "public",
    "public"
};

const char * expects_table[NUM_TESTS] = {
    "tb_a",
    "tb_a",
    "tb_a",
    "tb_a",
    "tb_a"
};

struct changeset changeset_expects[NUM_TESTS] = {
    {
        0,
        NULLREF, //( char ** ) &(expect_keys[0]),      // keys
        NULLREF, //( char ** ) &(expect_vals[0]),      // vals
        1,                                  // num_keys
        NULLREF, // NULL,
        NULLREF, // NULL,
        NULLREF, // NULL,
        0,                                  // num_columns
        NULLREF, //"public",                           // schema_name
        NULLREF, //"tb_a",                             // table_name
        4408,                               // xid
        PGC_DML_INSERT,                     // type
        0                                   // timestamp
    },
    {
        0,
        NULLREF, // ( char ** ) &(expect_keys[1]),
        NULLREF, // ( char ** ) &(expect_vals[1]),
        1,
        NULLREF, // ( char ** ) &(expect_columns[1]),
        NULLREF, // ( char ** ) &(expect_new[1]),
        NULLREF,
        3,
        NULLREF, //"public",
        NULLREF, //"tb_a",
        4408,
        PGC_DML_INSERT,
        1579120198
    },
    {
        0,
        NULLREF, // ( char ** ) &(expect_keys[2]),
        NULLREF, //( char ** ) &(expect_vals[2]),
        1,
        NULLREF,
        NULLREF,
        NULLREF,
        0,
        NULLREF, // "public",
        NULLREF, // "tb_a",
        4408,
        PGC_DML_INSERT,
        0
    },
    {
        0,
        NULLREF, //( char ** ) &(expect_keys[3]),
        NULLREF, //( char ** ) &(expect_vals[3]),
        1,
        NULLREF, //( char ** ) &(expect_columns[3]),
        NULLREF, //( char ** ) &(expect_new[3]),
        NULLREF, //( char ** ) &(expect_old[3]),
        3,
        NULLREF, //"public",
        NULLREF, //"tb_a",
        4408,
        PGC_DML_UPDATE,
        1579120198
    },
    {
        0,
        NULLREF, //( char ** ) &(expect_keys[4]),
        NULLREF, //( char ** ) &(expect_vals[4]),
        1,
        NULLREF, //( char ** ) &(expect_columns[4]),
        NULLREF, //( char ** ) &(expect_new[4]),
        NULLREF, //( char ** ) &(expect_old[4]),
        3,
        NULLREF, //"public",
        NULLREF, //"tb_a",
        4409,
        PGC_DML_UPDATE,
        1579120198
    }
};

/* End test definitions */
int main( void );
static void print_expects( struct changeset * ) __attribute__((unused));
static void print_array( changeset_array_ref_t, unsigned int );
static bool check_expects( struct changeset *, struct changeset * );

int main( void )
{
    struct changeset *       expects   = NULL;
    struct changeset *       received  = NULL;
    char *                   input     = NULL;
    uint8_t                  i         = 0;
    uint8_t                  j         = 0;
    uint8_t                  failed    = 0;
    changeset_string_ref_t * t         = NULL;
    char *                   temp      = NULL;
    char *                   exp_temp  = NULL;
    pg_ctblmgr_wal_level     wal_level = 0;
    changeset_ref_t          cs        = NULLREF;


    if( !slab_init() )
    {
        printf( "Failed to initialize slab\n" );
        return -1;
    }

    if( !initialize_changeset_context() )
    {
        printf( "Failed to initialized shared memory context(s)\n" );
        return -1;
    }

    for( i = 0; i < NUM_TESTS; i++ )
    {
        expects   = ( struct changeset * ) &((changeset_expects[i])) ;
        // Populate expects arrays
        if( expect_keys_len[i] > 0 )
        {
            expects->keys = ( changeset_array_ref_t ) rsmalloc(
                get_changeset_array_context(),
                sizeof( changeset_string_ref_t ) * expect_keys_len[i]
            );

            if( expects->keys == NULLREF )
            {
                printf( "Failed to allocate expects->keys[]" );
                return -1;
            }

            t = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), expects->keys );

            if( t == NULL )
                return -1;

            for( j = 0; j < expect_keys_len[i]; j++ )
            {
                exp_temp = ( ( char ** ) &(expect_keys[i]) )[j];
                t[j] = ( changeset_string_ref_t ) rsmalloc(
                    get_changeset_string_context(),
                    sizeof( char ) * ( strlen( exp_temp ) + 1 )
                );

                if( t[j] == NULLREF )
                {
                    printf( "Failed to allocate expects->keys[%u]", ( uint32_t ) j );
                    return -1;
                }

                temp = ( char * ) to_ptr( get_changeset_string_context(), t[j] );

                if( temp == NULL )
                    return -1;

                strncpy( temp, exp_temp, strlen( exp_temp ) );
                temp[strlen(exp_temp)] = '\0';
            }
        }

        if( expect_vals_len[i] > 0 )
        {
            expects->vals = ( changeset_array_ref_t ) rsmalloc(
                get_changeset_array_context(),
                sizeof( changeset_string_ref_t ) * expect_keys_len[i]
            );

            if( expects->vals == NULLREF )
            {
                printf( "Failed to allocate expects->vals[]" );
                return -1;
            }

            t = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), expects->vals );

            if( t == NULL )
                return -1;

            for( j = 0; j < expect_vals_len[i]; j++ )
            {
                exp_temp = ( ( char ** ) &(expect_vals[i]) )[j];
                t[j] = ( changeset_string_ref_t ) rsmalloc(
                    get_changeset_string_context(),
                    sizeof( char ) * ( strlen( exp_temp ) + 1 )
                );

                if( t[j] == NULLREF )
                {
                    printf( "Failed to allocate expects->keys[%u]", ( uint32_t ) j );
                    return -1;
                }

                temp = ( char * ) to_ptr( get_changeset_string_context(), t[j] );

                if( temp == NULL )
                    return -1;

                strncpy( temp, exp_temp, strlen( exp_temp ) );
                temp[strlen(exp_temp)] = '\0';
            }
        }

        if( expect_columns_len[i] > 0 )
        {
            expects->columns = ( changeset_array_ref_t ) rsmalloc(
                get_changeset_array_context(),
                sizeof( changeset_string_ref_t ) * expect_columns_len[i]
            );

            if( expects->columns == NULLREF )
            {
                printf( "Failed to allocate expects->columns[]" );
                return -1;
            }

            t = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), expects->columns );

            if( t == NULL )
                return -1;

            for( j = 0; j < expect_columns_len[i]; j++ )
            {
                exp_temp = ( ( char ** ) &(expect_columns[i]) )[j];
                t[j] = ( changeset_string_ref_t ) rsmalloc(
                    get_changeset_string_context(),
                    sizeof( char ) * ( strlen( exp_temp ) + 1 )
                );

                if( t[j] == NULLREF )
                {
                    printf( "Failed to allocate expects->keys[%u]", ( uint32_t ) j );
                    return -1;
                }

                temp = ( char * ) to_ptr( get_changeset_string_context(), t[j] );

                if( temp == NULL )
                    return -1;

                strncpy( temp, exp_temp, strlen( exp_temp ) );
                temp[strlen(exp_temp)] = '\0';
            }
        }

        if( expect_new_len[i] > 0 )
        {
            expects->new_vals = ( changeset_array_ref_t ) rsmalloc(
                get_changeset_array_context(),
                sizeof( changeset_string_ref_t ) * expect_new_len[i]
            );

            if( expects->new_vals == NULLREF )
            {
                printf( "Failed to allocate expects->new_vals[]" );
                return -1;
            }

            t = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), expects->new_vals );

            if( t == NULL )
                return -1;

            for( j = 0; j < expect_new_len[i]; j++ )
            {
                exp_temp = ( ( char ** ) &(expect_new[i]) )[j];
                t[j] = ( changeset_string_ref_t ) rsmalloc(
                    get_changeset_string_context(),
                    sizeof( char ) * ( strlen( exp_temp ) + 1 )
                );

                if( t[j] == NULLREF )
                {
                    printf( "Failed to allocate expects->keys[%u]", ( uint32_t ) j );
                    return -1;
                }

                temp = ( char * ) to_ptr( get_changeset_string_context(), t[j] );

                if( temp == NULL )
                    return -1;

                strncpy( temp, exp_temp, strlen( exp_temp ) );
                temp[strlen(exp_temp)] = '\0';
            }
        }

        if( expect_old_len[i] > 0 )
        {
            expects->old_vals = ( changeset_array_ref_t ) rsmalloc(
                get_changeset_array_context(),
                sizeof( changeset_string_ref_t ) * expect_old_len[i]
            );

            if( expects->old_vals == NULLREF )
            {
                printf( "Failed to allocate expects->old_vals[]" );
                return -1;
            }

            t = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), expects->old_vals );

            if( t == NULL )
                return -1;

            for( j = 0; j < expect_old_len[i]; j++ )
            {
                t[j] = ( changeset_string_ref_t ) rsmalloc(
                    get_changeset_string_context(),
                    sizeof( char ) * ( strlen( expect_old[i][j] ) + 1 )
                );

                if( t[j] == NULLREF )
                {
                    printf( "Failed to allocate expects->keys[%u]", ( uint32_t ) j );
                    return -1;
                }

                temp = ( char * ) to_ptr( get_changeset_string_context(), t[j] );

                if( temp == NULL )
                    return -1;

                strncpy( temp, expect_old[i][j], strlen( expect_old[i][j] ) );
                temp[strlen(expect_old[i][j])] = '\0';
            }
        }

        expects->schema_name = ( changeset_string_ref_t ) rsmalloc(
            get_changeset_string_context(),
            sizeof( expects_schema[i] ) + 1
        );

        exp_temp = ( char * ) to_ptr( get_changeset_string_context(), expects->schema_name );
        if( exp_temp == NULL )
            return -1;

        strncpy( exp_temp, expects_schema[i], strlen( expects_schema[i] ) );
        exp_temp[strlen(expects_schema[i])] = '\0';

        expects->table_name = ( changeset_string_ref_t ) rsmalloc(
            get_changeset_string_context(),
            sizeof( expects_table[i] ) + 1
        );
        exp_temp = ( char * ) to_ptr( get_changeset_string_context(), expects->table_name );
        if( exp_temp == NULL )
            return -1;

        strncpy( exp_temp, expects_table[i], strlen( expects_table[i] ) );
        exp_temp[strlen(expects_table[i])] = '\0';
        input     = ( char * ) tests[i];
        wal_level = wal_levels[i];
        //print_expects( expects );
        cs = json_to_changeset( input, wal_level );
        received = ( struct changeset * ) to_ptr( get_changeset_context(), cs );
        //print_expects( received );

        if( !check_expects( expects, received ) )
        {
            printf( "Test %u failed\n", i );
            failed++;
        }
    }

    if( failed == 0 )
    {
        printf( "All tests passed\n" );
        return 0;
    }

    return -1;
}

static void print_expects( struct changeset * cs )
{
    if( cs == NULL )
        return;

    printf(
        "Changeset we're expecting:\n keys: "
    );
    print_array( cs->keys, cs->num_keys );
    printf( "\n vals: " );
    print_array( cs->vals, cs->num_keys );
    printf( "\n num_keys: %u\n columns: ", cs->num_keys );
    print_array( cs->columns, cs->num_columns );
    printf( "\n old_vals: " );
    print_array( cs->old_vals, cs->num_columns );
    printf( "\n new_vals: " );
    print_array( cs->new_vals, cs->num_columns );
    printf(
        "\n num_columns: %u\n schema_name: %s\n table_name: %s\n xid: %lu\n type: %s\n timestamp: %lu\n",
        cs->num_columns,
        ( char * ) to_ptr( get_changeset_string_context(), cs->schema_name ),
        ( char * ) to_ptr( get_changeset_string_context(), cs->table_name ),
        cs->xid,
        cs->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
        cs->type == PGC_DML_INSERT ? "INSERT" :
        cs->type == PGC_DML_UPDATE ? "UPDATE" :
        cs->type == PGC_DML_DELETE ? "DELETE" :
        cs->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
        cs->type == PGC_DML_BEGIN ? "BEGIN" :
        cs->type == PGC_DML_ROLLBACK ? " ROLLBACK" :
        cs->type == PGC_DML_COMMIT ? "COMMIT" : "N/A",
        cs->timestamp
    );

    return;
}

static void print_array( changeset_array_ref_t arr, unsigned int num_elements )
{
    unsigned int i = 0;
    changeset_string_ref_t * array = NULL;
    char * elem = NULL;

    array = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), arr );

    if( num_elements == 0 )
    {
        printf( "NULL (%p)", array );
    }
    else if( num_elements > 0 && array != NULL )
    {
        for( i = 0; i < num_elements; i++ )
        {
            elem = ( char * ) to_ptr( get_changeset_string_context(), array[i] );
            printf( "'%s'", elem );

            if( i < num_elements - 1 )
            {
                printf( "," );
            }
        }
    }
    else
    {
        printf( "ERROR" );
    }

    return;
}

static bool check_expects( struct changeset * ex, struct changeset * cs )
{
    unsigned int i     = 0;
    unsigned int len_e = 0;
    unsigned int len_c = 0;
    changeset_string_ref_t * arr_e = NULL;
    changeset_string_ref_t * arr_c = NULL;
    char * arr_elem_e = NULL;
    char * arr_elem_c = NULL;

    if( ex == NULL && cs == NULL )
        return true;

    if( ex == NULL || cs == NULL )
    {
        if( ex == NULL )
            printf( "Expected NULL changeset\n" );

        if( cs == NULL )
            printf( "Received NULL changeset\n" );

        return false;
    }

    if( cs->num_keys != ex->num_keys )
    {
        printf(
            "Num keys mismatch: Ex: %u, Cs: %u",
            ex->num_keys,
            cs->num_keys
        );
        return false;
    }

    if(
          (
              ex->keys == NULLREF
           || cs->keys == NULLREF
           || ex->vals == NULLREF
           || cs->vals == NULLREF
          )
       && ex->num_keys != 0
      )
    {
        printf( "NULL keys array for changeset" );
        return false;
    }

    if( cs->num_keys > 0 )
    {
        for( i = 0; i < cs->num_keys; i ++ )
        {
            arr_e = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), ex->keys );
            arr_c = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), cs->keys );

            if( arr_e == NULL || arr_c == NULL )
            {
                printf( "One of changeset keys arrays dereferenced to null with num_keys > 0\n" );
                return false;
            }

            arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), arr_e[i] );
            arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), arr_c[i] );

            len_e = 0;

            if( arr_elem_e != NULL )
                len_e = strlen( arr_elem_e );

            len_c = 0;

            if( arr_elem_c != NULL )
                len_c = strlen( arr_elem_c );

            if(
                  len_e != len_c
               || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "keys mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    arr_elem_e,
                    i,
                    arr_elem_c
                );
                if( len_e != len_c )
                {
                    printf(
                        "(Keys lengths mismatched, E: %u C: %u)",
                        len_e,
                        len_c
                    );
                }
                return false;
            }

            arr_e = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), ex->vals );
            arr_c = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), cs->vals );

            if( arr_e == NULL || arr_c == NULL )
            {
                printf( "One of changeset keys arrays dereferenced to null with num_keys > 0\n" );
                return false;
            }

            arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), arr_e[i] );
            arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), arr_c[i] );

            len_e = 0;
            if( arr_elem_e != NULL )
                len_e = strlen( arr_elem_e );

            len_c = 0;
            if( arr_elem_c != NULL )
                len_c = strlen( arr_elem_c );

            if(
                  len_e != len_c
               || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    arr_elem_e,
                    i,
                    arr_elem_c
                );
                return false;
            }
        }
    }

    if( cs->num_columns != ex->num_columns )
    {
        printf(
            "num_columns mismatch: Ex: %u, Cs: %u",
            ex->num_columns,
            cs->num_columns
        );
        return false;
    }

    if(
          (
              ex->columns == NULLREF
           || cs->columns == NULLREF
          )
       && ex->num_columns != 0
      )
    {
        printf( "NULL column array for changeset!\n" );
        return false;
    }

    if(
          ( cs->old_vals == NULLREF && ex->old_vals != NULLREF )
       || ( cs->old_vals != NULLREF && ex->old_vals == NULLREF )
      )
    {
        printf( "old_vals mismatch, one is NULL\n" );
        return false;
    }

    if(
          ( cs->new_vals == NULLREF && ex->new_vals != NULLREF )
       || ( cs->new_vals != NULLREF && ex->new_vals == NULLREF )
      )
    {
        printf( "new_vals mismatch, one is NULL\n" );
        return false;
    }

    for( i = 0; i < cs->num_columns; i++ )
    {
        arr_e = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), ex->columns );
        arr_c = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), cs->columns );

        if( arr_e == NULL || arr_c == NULL )
        {
            printf( "Failed to dereference cs/ex ->columns[] arrays\n" );
            return false;
        }

        arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), arr_e[i] );
        arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), arr_c[i] );

        if( arr_elem_e == NULL || arr_elem_c == NULL )
        {
            printf( "Failed to dereference columns[%lu]\n", ( uint64_t ) i );
            return false;
        }

        len_e = strlen( arr_elem_e );
        len_c = strlen( arr_elem_c );

        if(
              len_e != len_c
           || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
          )
        {
            printf(
                "columns mismatch: Ex{%u]: %s, Cs[%u]: %s",
                i,
                arr_elem_e,
                i,
                arr_elem_c
            );
            return false;
        }

        if( ex->old_vals != NULLREF )
        {
            arr_e = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), ex->old_vals );
            arr_c = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), cs->old_vals );

            if( arr_e == NULL || arr_c == NULL )
            {
                printf( "Failed to dereference cs/ex ->old_vals[] arrays\n" );
                return false;
            }

            arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), arr_e[i] );
            arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), arr_c[i] );

            if( arr_elem_e == NULL || arr_elem_c == NULL )
            {
                printf( "Failed to dereference old_vals[%lu]\n", ( uint64_t ) i );
                return false;
            }

            len_e = strlen( arr_elem_e );
            len_c = strlen( arr_elem_c );

            if(
                   len_e != len_c
                || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "old_vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    arr_elem_e,
                    i,
                    arr_elem_c
                );
                return false;
            }
        }

        if( ex->new_vals != NULLREF )
        {
            arr_e = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), ex->new_vals );
            arr_c = ( changeset_string_ref_t * ) to_ptr( get_changeset_array_context(), cs->new_vals );

            if( arr_e == NULL || arr_c == NULL )
            {
                printf( "Failed to dereference cs/ex ->new_vals[] arrays\n" );
                return false;
            }

            arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), arr_e[i] );
            arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), arr_c[i] );

            if( arr_elem_e == NULL || arr_elem_c == NULL )
            {
                printf( "Failed to dereference new_vals[%lu]\n", ( uint64_t ) i );
                return false;
            }

            len_e = strlen( arr_elem_e );
            len_c = strlen( arr_elem_c );

            if(
                   len_e != len_c
                || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "new_vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    arr_elem_e,
                    i,
                    arr_elem_c
                );
                return false;
            }
        }
    }

    if( ex->schema_name == NULLREF || cs->schema_name == NULLREF )
    {
        printf( "Schema_name seems to be unallocated\n" );
        if( ex->schema_name == NULLREF )
            printf( "Expect has null schema name\n" );
        if( cs->schema_name == NULLREF )
            printf( "Changeset has null schema name\n" );
        return false;
    }

    arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), ex->schema_name );
    arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), cs->schema_name );

    if( arr_elem_e == NULL || arr_elem_c == NULL )
    {
        printf( "schema_name dereferenced to null for ex/cs\n" );
        return false;
    }
    len_e = strlen( arr_elem_e );
    len_c = strlen( arr_elem_c );

    if(
           len_e != len_c
        || strncmp( arr_elem_e, arr_elem_c, MIN( len_e, len_c ) ) != 0
      )
    {
        printf(
            "schema_name mismatch: Ex: %s, Cs: %s\n",
            arr_elem_e,
            arr_elem_c
        );
        return false;
    }

    if( ex->table_name == NULLREF )
    {
        len_e = 0;
    }
    else
    {
        arr_elem_e = ( char * ) to_ptr( get_changeset_string_context(), ex->table_name );
        len_e = strlen( arr_elem_e );
    }

    if( cs->table_name == NULLREF )
    {
        len_c = 0;
    }
    else
    {
        arr_elem_c = ( char * ) to_ptr( get_changeset_string_context(), cs->table_name );
        len_c = strlen( arr_elem_c );
    }

    if(
          len_e != len_c
       || strncmp( arr_elem_c, arr_elem_e, MIN( len_e, len_c ) ) != 0
      )
    {
        printf(
            "table_name mismatch: Ex: %s, Cs: %s",
            arr_elem_e,
            arr_elem_c
        );
        return false;
    }

    if( cs->xid != ex->xid )
    {
        printf(
            "xid mismatch: Ex: %lu, Cs: %lu",
            ex->xid,
            cs->xid
        );
        return false;
    }

    if( cs->type != ex->type )
    {
        printf(
            "type mismatch: Ex: %s, Cs: %s",
            ex->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
            ex->type == PGC_DML_INSERT ? "INSERT" :
            ex->type == PGC_DML_UPDATE ? "UPDATE" :
            ex->type == PGC_DML_DELETE ? "DELETE" :
            ex->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
            ex->type == PGC_DML_COMMIT ? "COMMIT" :
            ex->type == PGC_DML_ROLLBACK ? "ROLLBACK" :
            ex->type == PGC_DML_BEGIN ? "BEGIN" : "N/A",
            cs->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
            cs->type == PGC_DML_INSERT ? "INSERT" :
            cs->type == PGC_DML_UPDATE ? "UPDATE" :
            cs->type == PGC_DML_DELETE ? "DELETE" :
            cs->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
            cs->type == PGC_DML_COMMIT ? "COMMIT" :
            cs->type == PGC_DML_ROLLBACK ? "ROLLBACK" :
            cs->type == PGC_DML_BEGIN ? "BEGIN" : "N/A"
        );
        return false;
    }

    if( ex->timestamp != cs->timestamp )
    {
        printf(
            "timestamp mismatch: Ex: %lu, Cs: %lu",
            ex->timestamp,
            cs->timestamp
        );
        return false;
    }

    return true;
}
