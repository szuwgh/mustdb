\echo Use "CREATE EXTENSION pgvectorbase" to load this file. \quit

CREATE FUNCTION pgvectorbase_version()
RETURNS text
AS 'MODULE_PATHNAME', 'pgvectorbase_version'
LANGUAGE C STRICT PARALLEL SAFE;


CREATE FUNCTION vb_ivfflathandler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'vb_ivfflathandler'
LANGUAGE C;


CREATE ACCESS METHOD vb_ivf2 TYPE INDEX HANDLER vb_ivfflathandler;

