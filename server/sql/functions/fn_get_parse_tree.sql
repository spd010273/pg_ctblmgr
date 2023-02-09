CREATE OR REPLACE FUNCTION fn_get_parse_tree( in_sql TEXT )
RETURNS TEXT AS
 'pg_ctblmgr.so', 'get_parse_tree'
LANGUAGE C IMMUTABLE PARALLEL SAFE;
