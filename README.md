pg_ctblmgr
----------

Logical Replication Based, Asynchronous Incremental  Materialized Views

# Summary

pg_ctblmgr is a PostgreSQL extension that impelments logical replications based asynchronous materialized views. This extension consists of a server-side Logical Decoder Plugin and a service. The service uses the decoded WAL to update tuples impacted by changes to the base tables (tables from which they draw data). The service also:

* Performs commanded full refreshes (similar to REFRESH MATERIALIZED VIEW)
* Maintains statistics on each 'cache table' it maintains.
* Runs a separate process per 'cache table'.

Each 'cache table' is implemented as a real-life table, rather than patching in a new subtype of materialized view.

An asynchronous approach was taken because this allows the extension to be decoupled from the database primar(y|ies), moving processing overhead out-of-band. It also allows for the extension to function on vanilla, out-of-the-box PostgreSQL installations without the need to recompilation or patching. The caveat is that while the originating transaction is not delayed by maintenance overhead of the 'cache tables', there will be some measurable lag until the 'cache table' reflects the changes made to the base tables in said transaction. This is a function of the number of tuples modified in a given transaction and the complexity of the 'cache table's' definition.

# Getting Started

## Prerequisites:

This extension requires the following:

* PostgreSQL 9.4 or better
* git, gcc, make, and PostgreSQL development libraries

## Installing

Once the extension code is checked out, it can be built and installed with

```bash
make && su - postgres -c 'cd /path/to/pg_ctblmgr/ && make install'
```

Prior to beginning, you should verify that pg_Config is in the user's PATH, and that it matches the version of the server that is running.

Once these steps are complete, the extension installation can be finalized by logging into the database and running

```SQL
CREATE EXTENSION pg_ctblmgr;
```

# Versions

This is a replacement extension for the Perl and asynchronous LISTEN/NOTIFY based tblmgr.

For more information, see CHANGELOG.md

# License

pg_ctblmgr is released under the PostgreSQL license. For more details about this license, please see LICENSE

# Thanks

Thank you to my employer for alloting the time and resources to develop this project.

Thank you to the PostgreSQL project for maintaining and documenting such well written, structured, and understandable code.

 - Portions of the PostgreSQL code base were used as a model for the shm implementation, specifically the shims for system level shared memory interfaces.

Thank you to the maintainers of JSMN for providing a fast, lightweight, and easy-to-use JSON parsing library.
