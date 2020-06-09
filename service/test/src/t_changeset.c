#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "../src/lib/changeset.h"

/* Here we define the test changeset string, their WAL level, and the expected
 * output structure */
#define NUM_TESTS 3

const char * tests[NUM_TESTS] = {
    "{\"d\":\"I\",\"x\":\"4408\",\"s\":\"public\",\"t\":\"tb_a\",\"key\":{\"foo\":1}}",
    "{\"type\":\"INSERT\",\"xid\":\"4408\",\"timestamp\":\"2020-01-15 15:29:58.892742-05\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":4},\"data\":{\"new\":{\"foo\":4,\"bar\":5,\"baz\":6}}}",
    "{\"type\":\"INSERT\",\"xid\":\"4408\",\"schema_name\":\"public\",\"table_name\":\"tb_a\",\"key\":{\"foo\":1}}"
};

const pg_ctblmgr_wal_level wal_levels[NUM_TESTS] = {
    PGC_WAL_MINIMAL,
    PGC_WAL_FULL,
    PGC_WAL_REDUCED
};

const char * expect_keys[NUM_TESTS] = {
    "foo",
    "foo",
    "foo"
};

const char * expect_vals[NUM_TESTS] = {
    "1",
    "1",
    "1"
};

const char * expect_columns[NUM_TESTS][3] = {
    { NULL },
    { "foo", "bar", "baz" },
    { NULL },
};

const char * expect_new[NUM_TESTS][3] = {
    { NULL },
    { "4", "5", "6" },
    { NULL }
};

const char * expect_old[NUM_TESTS][3] = {
    { NULL },
    { NULL },
    { NULL }
};

const struct changeset changeset_expects[NUM_TESTS] = {
    {
        ( char ** ) &(expect_keys[0]),      // keys
        ( char ** ) &(expect_vals[0]),      // vals
        1,                                  // num_keys
        NULL,
        NULL,
        NULL,
        0,                                  // num_columns
        "public",                           // schema_name
        "tb_a",                             // table_name
        4408,                               // xid
        PGC_DML_INSERT,                     // type
        0                                   // timestamp
    },
    {
        ( char ** ) &(expect_keys[1]),
        ( char ** ) &(expect_vals[1]),
        1,
        ( char ** ) &(expect_columns[1]),
        ( char ** ) &(expect_new[1]),
        NULL,
        3,
        "public",
        "tb_a",
        4408,
        PGC_DML_INSERT,
        1579120198
    },
    {
        ( char ** ) &(expect_keys[2]),
        ( char ** ) &(expect_vals[2]),
        1,
        NULL,
        NULL,
        NULL,
        0,
        "public",
        "tb_a",
        4408,
        PGC_DML_INSERT,
        0
    }
};

/* End test definitions */
int main( void );
static void print_expects( struct changeset * );
static void print_array( char **, unsigned int );
static bool check_expects( struct changeset *, struct changeset * );

int main( void )
{
    struct changeset *   expects   = NULL;
    struct changeset *   received  = NULL;
    char *               input     = NULL;
    unsigned int         i         = 0;
    unsigned int         failed    = 0;
    pg_ctblmgr_wal_level wal_level = 0;

    for( i = 0; i < NUM_TESTS; i++ )
    {
        expects   = ( struct changeset * ) &((changeset_expects[i])) ;
        input     = ( char * ) tests[i];
        wal_level = wal_levels[i];
        print_expects( expects );

        printf(
            "Decoding (%s): %s\n",
            wal_level == PGC_WAL_FULL
                ? "FULL"
                : wal_level == PGC_WAL_REDUCED
                ? "REDUCED"
                : wal_level == PGC_WAL_MINIMAL
                ? "MINIMAL"
                : "Unknown",
            input
        );

        received = json_to_changeset( input, wal_level );
        print_expects( received );
        if( !check_expects( expects, received ) )
        {
            printf( "Test %u failed\n", i );
            failed++;
        }
    }

    if( failed == 0 )
    {
        printf( "All tests passed\n" );
    }

    return 0;
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
        cs->schema_name,
        cs->table_name,
        cs->xid,
        cs->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
        cs->type == PGC_DML_INSERT ? "INSERT" :
        cs->type == PGC_DML_UPDATE ? "UPDATE" :
        cs->type == PGC_DML_DELETE ? "DELETE" :
        cs->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
        cs->type == PGC_DML_TX_BARRIER ? "TRANSACTION" : "N/A",
        cs->timestamp
    );

    return;
}

static void print_array( char ** arr, unsigned int num_elements )
{
    unsigned int i = 0;

    if( num_elements == 0 )
    {
        printf( "NULL (%p)", arr );
    }
    else if( num_elements > 0 && arr != NULL )
    {
        for( i = 0; i < num_elements; i++ )
        {
            printf( "'%s'", arr[i] );

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
              ex->keys == NULL
           || cs->keys == NULL
           || ex->vals == NULL
           || cs->vals == NULL
          )
       && ex->num_keys != 0
      )
    {
        printf( "NULL keys array for changeset" );
        return false;
    }
    
    for( i = 0; i < cs->num_keys; i ++ )
    {
        len_e = strlen( ex->keys[i] );
        len_c = strlen( cs->keys[i] );

        if(
              len_e != len_c
           || strncmp( ex->keys[i], cs->keys[i], MIN( len_e, len_c ) ) != 0
          )
        {
            printf(
                "keys mismatch: Ex[%u]: %s, Cs[%u]: %s",
                i,
                ex->keys[i],
                i,
                cs->keys[i]
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
    
        len_e = strlen( ex->vals[i] );
        len_c = strlen( cs->vals[i] );

        if(
              len_e != len_c
           || strncmp( ex->vals[i], cs->vals[i], MIN( len_e, len_c ) ) != 0
          )
        {
            printf(
                "vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                i,
                ex->vals[i],
                i,
                cs->vals[i]
            );
            return false;
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
              ex->columns == NULL
           || cs->columns == NULL
          )
       && ex->num_columns != 0
      )
    {
        printf( "NULL column array for changeset!\n" );
        return false;
    }
    
    if(
          ( cs->old_vals == NULL && ex->old_vals != NULL )
       || ( cs->old_vals != NULL && ex->old_vals == NULL )
      )
    {
        printf( "old_vals mismatch, one is NULL\n" );
        return false;
    }

    if(
          ( cs->new_vals == NULL && ex->new_vals != NULL )
       || ( cs->new_vals != NULL && ex->new_vals == NULL )
      )
    {
        printf( "new_vals mismatch, one is NULL\n" );
        return false;
    }

    for( i = 0; i < cs->num_columns; i++ )
    {
        len_e = strlen( ex->columns[i] );
        len_c = strlen( cs->columns[i] );

        if(
              len_e != len_c
           || strncmp( ex->columns[i], cs->columns[i], MIN( len_e, len_c ) ) != 0
          )
        {
            printf(
                "columns mismatch: Ex{%u]: %s, Cs[%u]: %s",
                i,
                ex->columns[i],
                i,
                cs->columns[i]
            );
            return false;
        }

        if( ex->old_vals != NULL )
        {
            len_e = strlen( ex->old_vals[i] );
            len_c = strlen( cs->old_vals[i] );

            if(
                   len_e != len_c
                || strncmp( ex->old_vals[i], cs->old_vals[i], MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "old_vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    ex->old_vals[i],
                    i,
                    cs->old_vals[i]
                );
                return false;
            }
        }

        if( ex->new_vals != NULL )
        {
            len_e = strlen( ex->new_vals[i] );
            len_c = strlen( cs->new_vals[i] );

            if(
                   len_e != len_c
                || strncmp( ex->new_vals[i], cs->new_vals[i], MIN( len_e, len_c ) ) != 0
              )
            {
                printf(
                    "new_vals mismatch: Ex[%u]: %s, Cs[%u]: %s",
                    i,
                    ex->new_vals[i],
                    i,
                    cs->new_vals[i]
                );
                return false;
            }
        }
    }

    len_e = strlen( ex->schema_name );
    len_c = strlen( cs->schema_name );
    
    if(
           len_e != len_c
        || strncmp( cs->schema_name, ex->schema_name, MIN( len_e, len_c ) ) != 0
      )
    {
        printf(
            "schema_name mismatch: Ex: %s, Cs: %s",
            ex->schema_name,
            cs->schema_name
        );
        return false;
    }

    if( ex->table_name == NULL )
    {
        len_e = 0;
    }
    else
    {
        len_e = strlen( ex->table_name );
    }

    if( cs->table_name == NULL )
    {
        len_c = 0;
    }
    else
    {
        len_c = strlen( cs->table_name );
    }

    if(
          len_e != len_c
       || strncmp( cs->table_name, ex->table_name, MIN( len_e, len_c ) ) != 0
      )
    {
        printf(
            "table_name mismatch: Ex: %s, Cs: %s",
            ex->table_name,
            cs->table_name
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
            ex->type == PGC_DML_TX_BARRIER ? "TRANSACTION" : "N/A",
            cs->type == PGC_DML_UNINITIALIZED ? "UNINITIALIZED" :
            cs->type == PGC_DML_INSERT ? "INSERT" :
            cs->type == PGC_DML_UPDATE ? "UPDATE" :
            cs->type == PGC_DML_DELETE ? "DELETE" :
            cs->type == PGC_DML_TRUNCATE ? "TRUNCATE" :
            cs->type == PGC_DML_TX_BARRIER ? "TRANSACTION" : "N/A"
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
