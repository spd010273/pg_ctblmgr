pg_ctblmgr
----------

Logical Replication Based, Asynchronous Incremental  Materialized Views

# Summary

pg_ctblmgr is a PostgreSQL extension that impelments logical replications based asynchronous materialized views. This extension consists of a server-side Logical Decoder Plugin and a service. The service uses the decoded WAL to update tuples impacted by changes to the base tables (tables from which they draw data). The service also:

* Performs commanded full refreshes (similar to REFRESH MATERIALIZED VIEW)
* Maintains statistics on each 'cache table' it maintains.
* Runs a separate process per 'cache table'.

Each 'cache table' is implemented as a real-life table, technically making it materialized.

# Getting Started

## Prerequisites:

This extension requires the following:

* PostgreSQL 9.4 or better
* gcc, make, and PostgreSQL development libraries

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
