Development Notes
-----------------

# Theory of Operation

The `pg_ctblmgr` extension must acheive a few objectives given a user-provided query:

1. Determine the tables used in the query
2. Place triggers that let us know when changes occur to these tables
3. Once a change occurs, we must:
    a. Determine where that change can be bound into the parsed query from (1).
    b. Generate a filtered SELECT, followed by DML that, using the SELECT, can perform a DELETE, UPDATE, and INSERT on the relevent cache table.
    c. Generate a temp table using the filtered select.
    d. Perform either a slow or fast delete.
    e. Perform either a regular or bulk update.
    f. Perform the insert (iff bulk update has not occurred).

Step 3a is the most complicated portion of the code, as it bridges the near-infinite expressiveness of the SQL syntax with the need to efficiently and correctly filter the output of the query to contain only the effected rows.

# Query Parsing

`pg_ctblmgr` parses queries by hooking into PostgreSQL's query parser via `fn_get_parse_tree`. This function converts PostgreSQL's internal query parse tree into JSONB output by converting Node / NodeTag structure attributes into JSON attributes.

From there, `pg_ctblmgr` parses the JSON to determine a few things:

- What tables are involved in a queru
- Bind positions and syntax (Example: Bind to a WHERE clause versus a query with no where clause)
    - This includes subqueries, nested and recursive CTEs, UNIONs, etc.
- Appropriate filtering for base tables which are outer joined ( right / left / full outer ) to the query.

Once a query is parsed, the bind positions is cached for each worker. When it comes time to bind arbitrary parameters, the worker knows where each table's relative bind positions are, for example.

Given
```sql
WITH some_common_table_expression AS
(
    SELECT f.foo,
           b.bar,
           b.baz
      FROM public.tb_foo f
INNER JOIN public.tb_bar b
        ON b.bar = f.bar
)
    SELECT COUNT( DISTINCT bz.foobarbaz ) AS foobarbaz_count,
           cte.foo,
           cte.bar
      FROM some_common_table_expression cte
 LEFT JOIN public.tb_baz bz
        ON bz.baz = cte.baz
  GROUP BY cte.foo, cte.bar
```

The backend responsible for this cache table will know that:
- It should be concerned with DML to the folloiwng tables: `public.tb_foo`, `public.tb_bar`, and `public.tb_baz`
- `tb_foo` and `tb_bar` can be filtered by adding a `WHERE` clause inside the common table expression
- `tb_baz` is involved in an outer join to `tb_bar`, and that `tb_baz` can only be accurately filtered by applying a where clause in the form of `WHERE b.baz IN( SELECT baz FROM public.tb_baz WHERE baz = ANY( ARRAY[ <changed tb_baz keys> ]::INTEGER[] )` to the interior of the common table expression

Following are examples of the query, given the change of primary key 1 for each table:
`public.tb_foo`:
```sql
WITH some_common_table_expression AS
(
    SELECT f.foo,
           b.bar,
           b.baz
      FROM public.tb_foo f
INNER JOIN public.tb_bar b
        ON b.bar = f.bar
     WHERE f.foo = '1'::int4
)
    SELECT COUNT( DISTINCT bz.foobarbaz ) AS foobarbaz_count,
           cte.foo,
           cte.bar
      FROM some_common_table_expression cte
 LEFT JOIN public.tb_baz bz
        ON bz.baz = cte.baz
  GROUP BY cte.foo, cte.bar
```

`public.tb_bar`:
```sql
WITH some_common_table_expression AS
(
    SELECT f.foo,
           b.bar,
           b.baz
      FROM public.tb_foo f
INNER JOIN public.tb_bar b
        ON b.bar = f.bar
     WHERE b.bar = '1'::int4
)
    SELECT COUNT( DISTINCT bz.foobarbaz ) AS foobarbaz_count,
           cte.foo,
           cte.bar
      FROM some_common_table_expression cte
 LEFT JOIN public.tb_baz bz
        ON bz.baz = cte.baz
  GROUP BY cte.foo, cte.bar
```

And finally, as described above, `public.tb_bar`:
```sql
WITH some_common_table_expression AS
(
    SELECT f.foo,
           b.bar,
           b.baz
      FROM public.tb_foo f
INNER JOIN public.tb_bar b
        ON b.bar = f.bar
     WHERE b.baz IN( SELECT baz FROM public.tb_baz WHERE baz = '1'::int4 )
)
    SELECT COUNT( DISTINCT bz.foobarbaz ) AS foobarbaz_count,
           cte.foo,
           cte.bar
      FROM some_common_table_expression cte
 LEFT JOIN public.tb_baz bz
        ON bz.baz = cte.baz
  GROUP BY cte.foo, cte.bar
```

This odd binding leads us to:

## The Outer Join Case

Outer joins (`FULL OUTER`, `LEFT`, `RIGHT`) are a special case where we need to determine the appropriate way to filter the set. Direct filtering is incorrect (adding `WHERE <outer_table_primary_key> = <changed_key>`), as it converts an outer join to an inner join. To circumvent this, pg_ctblmgr generates a subselect statement which finds the keys used to join the outer relation based on that outer relations primary key, as demonstrated above.

## Anti Joins

Anti joins have not been tested nor implemented currently. They would need to be detected by checking if an outer relation is located, and that outer relation has one or more `IS NULL` predicate or where clause entries present. Once an anti-joined relation has been found, the filtering could be modified to be within the outer join's predicate if needed.

# Optimizations:

The alternate execution paths for delete, update, and insert operations are optimizations based on the following conditions:

## Slow Delete vs Fast Delete

Deletion from the target cache table is already a tricky problem. Given some change in the base tables, the query may ( or may not ) contain rows on its output. If the query does not contain rows on its output, then a delete must be performed. If the output is missing the row that is to be deleted, how do you know which row to delete?

Enter fast delete. Fast deletion uses a historic database connection (connection opened and held at a snapshot that occurred sometime in the past) to determine if a row was removed. This leverages the fast that insert operations occur last and deletes all tuples that appear in the output of the query, as run on the historic connection.

Slow deletion is the alternative solution. It runs the unfiltered query against the current timeline, and comparing the output with the current state of the cache table, it can be determined which row(s) should be deleted.

As part of the fast delete mechanism, the parent process handles the maintenance of `MAX_XID_LENGTH` historic connections that are `XID_MAP_SPREAD` seconds apart. The snapshot information is stored in shared memory (`XID_MAP`) and, when needed, a worker process imports the 'youngest'historic snapshot it can, given the xid of the change it is processing.

## Regular vs Bulk Update

In some cases, an UPDATE statement ran over hundreds of thousands (or millions) of rows is slower than the corresponding (and logically identical) DELETE and INSERT operation. If `pg_ctblmgr` detects that there are more than `BULK_ACTION_CUTOFF` changed tuples, it will opt to perform a bulk update ( DELETE followed by INSERT ) on the cache table. Since these operations are performed using changed tuples, the subsequent INSERT operation can be skipped (no need to INSERT twice).

## Temp Tables vs Common Table Expressions

`pg_ctblmgr` will attempt to track, when possible, the correlation of how many rows in the output change per changed row of a given base table. These counts are stored and averaged as countable requests come in. Countable requests are requests where the changes being processed are sourced from a single table.

Once sufficient count data is stored, `pg_ctblmgr` can make accurate approximations for how many rows could be generated by the definition query, given the changes to the base tables. This leads to conditional logic like the use of temp tables or common table expressions.

The purpose of this optimization is to maintain query execution speed while also reducing the number of MATERIALIZE operations (`CREATE TEMP TABLE ... ` or `WITH <CTE> AS MATERIALIZED`

## NOT NULL and unique indicies

The DML that is generated for maintenance of the cache tables can be further optimized by accounting for unique key members that are not NULL.
 This lets `pg_ctblmgr` use the much faster (and indexable via BTREE) equality operations rather than equality with null considerations:

```
     ...
FROM <relation A> a
JOIN <relation B> b
  ON a.key_1 = b.key_1
 AND a.key_2 = b.key_2
 AND a.key_3 = b.key_3
     ...
```

versus

```
FROM <relation A> a
JOIN <relation B> b
  ON ( ( a.key_1 IS NULL AND b.key_1 IS NULL ) OR ( a.key_1 = b.key_1 ) )
 AND ( ( a.key_2 IS NULL AND b.key_2 IS NULL ) OR ( a.key_2 = b.key_2 ) )
 AND ( ( a.key_3 IS NULL AND b.key_2 IS NULL ) OR ( a,key_3 = b.key_3 ) )
```

