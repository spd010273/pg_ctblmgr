pg_ctblmgr
----------

Trigger-Based, Asynchronous Incremental  Materialized Views

# Summary

pg_ctblmgr is a PostgreSQL extension that impelments trigger-based asynchronous materialized views. This extension consists of a server-side SQL notifiers and a service. The service uses notifications to update tuples impacted by changes to the base tables (tables from which they draw data). The service also:

* Performs commanded full refreshes (similar to REFRESH MATERIALIZED VIEW)
* Maintains statistics on each 'cache table' it maintains.
* Runs a separate process per 'cache table'.

Each 'cache table' is implemented as a real-life table, rather than patching in a new subtype of materialized view.

An asynchronous approach was taken because this allows the extension to be decoupled from the database primar(y|ies), moving processing overhead out-of-band. It also allows for the extension to function on vanilla, out-of-the-box PostgreSQL installations without the need to recompilation or patching. The caveat is that while the originating transaction is not delayed by maintenance overhead of the 'cache tables', there will be some measurable lag until the 'cache table' reflects the changes made to the base tables in said transaction. This is a function of the number of tuples modified in a given transaction and the complexity of the 'cache table's' definition.

pg_ctblmgr uses triggers to determine which keys were modified following an arbitrary DML statement. pg_ctblmgr then uses query parsing hooks to determine how to apply the key to the cache table definition as a WHERE clause element. This filtered subset of data tells the extension how to apply the changes to the cache table representation of the data.


## Details

pg_ctblmgr will generate notification events indicating when rows related to a cache table have changed, and determine how those changes impact cache tables under its control. The parent process is in charge of segment distribution, and one worker is assigned to each cache table. Each worker acknowledges WAL segments by their LSN as they are applies, and a given WAL segment is acknowledged with the primary server once all workers using that segment have acknowledged that it was applied.

pg_ctblmgr also monitors cache table definitions for modification, and will replace the cache table if a change to its definition is detected.

In steady-state operation, pg_ctblmgr maintains a list of historic transaction snapshots, which are used to observe the state of query output before a WAL change was applied to the database. This is helpful in determining if an operation visible in WAL results in a delete to a cache table, and is much faster than running an unfiltered definition query and full outer join operation to a cache table to determine if rows need to be removed.

# Getting Started

## Prerequisites:

This extension requires the following:

* PostgreSQL 9.4 or better
* git, gcc, make, and PostgreSQL development libraries
* perl along with DBD::Pg, Data::Search, JSON::XS, POSIX, Readonly, Carp, English, Params::Validate, DBI, IO::Select, IO::Handle, Getopt::Std, Time::HiRes, Cwd, IPC::Shareable, Data::Dumper, FindBin, Perl6::Export::Attrs, IO::Interactive.

## Installing

Once the extension code is checked out, it can be built and installed with

```bash
make && sudo make install'
```

Prior to beginning, you should verify that pg_Config is in the user's PATH, and that it matches the version of the server that is running.

Once these steps are complete, the extension installation can be finalized by logging into the database and running

```SQL
CREATE EXTENSION pg_ctblmgr;
```

## Running

pg_ctblmgr relies on an asynchronous service to maintain cache table state. This service is located in service/pg_ctblmgr.pl

This daemon requires the connection parameters to the database cluster hosting the extension.

# Versions

This is a replacement extension for the single-threaded tblmgr.

For more information, see CHANGELOG.md

# License

pg_ctblmgr is released under the PostgreSQL license. For more details about this license, please see LICENSE

# Thanks

Thank you to my employer for alloting the time and resources to develop this project.

Thank you to the PostgreSQL project for maintaining and documenting such well written, structured, and understandable code.

 - Portions of the PostgreSQL code base were used as a model for the shm implementation, specifically the shims for system level shared memory interfaces.

Thank you to the maintainers of JSMN for providing a fast, lightweight, and easy-to-use JSON parsing library.
