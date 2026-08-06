\echo Use "CREATE EXTENSION pg_mustdb" to load this file. \quit

CREATE FUNCTION pg_mustdb_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_mustdb_version'
LANGUAGE C STRICT PARALLEL SAFE;


CREATE FUNCTION mustdb_ivfflathandler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'mustdb_ivfflathandler'
LANGUAGE C;


CREATE ACCESS METHOD mustdb_ivf2 TYPE INDEX HANDLER mustdb_ivfflathandler;

