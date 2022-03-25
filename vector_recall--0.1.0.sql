SET search_path = public;
-- SET client_min_messages TO DEBUG3;

CREATE TYPE __vector_index_search_results AS (query_vector REAL[], query_idx BIGINT, vector_idxs BIGINT[], distances REAL[]);
CREATE TYPE __topk_merge_result AS (idxs BIGINT[], distances REAL[]);

CREATE OR REPLACE FUNCTION array_1d_extend_transfn(internal, REAL[])
    RETURNS internal
    AS 'MODULE_PATHNAME', 'array_1d_extend_transfn'
    LANGUAGE C;

CREATE OR REPLACE FUNCTION array_1d_extend_finalfn(internal)
    RETURNS REAL[]
    AS 'MODULE_PATHNAME', 'array_1d_extend_finalfn'
    LANGUAGE C;

CREATE AGGREGATE array_1d_extend(REAL[]) (
    SFUNC = array_1d_extend_transfn,
    STYPE = internal,
    FINALFUNC = array_1d_extend_finalfn
);


CREATE OR REPLACE FUNCTION faiss_index_create(dim INT, index_desc TEXT = 'IDMap,HNSW32,Flat', metric_type INT = 1)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'faiss_index_create'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION faiss_index_train(faiss_index BYTEA, vectors REAL[], dim INT)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'faiss_index_train'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION faiss_index_add(faiss_index BYTEA, vectors REAL[], dim INT, vector_idxs BIGINT[] = NULL)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'faiss_index_add'
    LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION faiss_index_set_runtime_parameters(faiss_index BYTEA, runtime_parameters TEXT)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'faiss_index_set_runtime_parameters'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION faiss_index_reset(faiss_index BYTEA)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'faiss_index_reset'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION create_index_transfn(internal, vector REAL[], index_desc TEXT, vector_idx BIGINT, metric_type INT, runtime_parameters TEXT)
    RETURNS internal
    AS 'MODULE_PATHNAME', 'create_index_transfn'
    LANGUAGE C;

CREATE OR REPLACE FUNCTION create_index_finalfn(internal)
    RETURNS BYTEA
    AS 'MODULE_PATHNAME', 'create_index_finalfn'
    LANGUAGE C;

CREATE AGGREGATE create_index_agg(vector REAL[], index_desc TEXT, vector_idx BIGINT, metric_type INT, runtime_parameters TEXT) (
    SFUNC = create_index_transfn,
    STYPE = internal,
    FINALFUNC = create_index_finalfn
);

CREATE OR REPLACE FUNCTION faiss_index_search(faiss_index BYTEA, query_vectors REAL[], dim INT, topk INT, query_idxs BIGINT[] = NULL, preserve_vector bool = TRUE, faiss_index_key TEXT = NULL)
    RETURNS SETOF __vector_index_search_results
    AS 'MODULE_PATHNAME', 'faiss_index_search'
    LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION faiss_index_range_search(faiss_index BYTEA, query_vectors REAL[], dim INT, radius REAL, query_idxs BIGINT[] = NULL, preserve_vector bool = TRUE, faiss_index_key TEXT = NULL)
    RETURNS SETOF __vector_index_search_results
    AS 'MODULE_PATHNAME', 'faiss_index_range_search'
    LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION topk_merge_transfn(internal, idxs BIGINT[], distance REAL[], topk INT)
    RETURNS internal
    AS 'MODULE_PATHNAME', 'topk_merge_transfn'
    LANGUAGE C;

CREATE OR REPLACE FUNCTION topk_merge_finalfn(internal)
    RETURNS __topk_merge_result
    AS 'MODULE_PATHNAME', 'topk_merge_finalfn'
    LANGUAGE C;

CREATE AGGREGATE topk_merge(idxs BIGINT[], distance REAL[], topk INT) (
    SFUNC = topk_merge_transfn,
    STYPE = internal,
    FINALFUNC = topk_merge_finalfn
);

CREATE OR REPLACE FUNCTION reset_cache(capacity BIGINT)
    RETURNS BOOLEAN
    EXECUTE ON ALL SEGMENTS
    AS 'MODULE_PATHNAME', 'reset_cache'
    LANGUAGE C;

CREATE OR REPLACE FUNCTION total_charge_cache()
    RETURNS BIGINT
    EXECUTE ON ALL SEGMENTS
    AS 'MODULE_PATHNAME', 'total_charge_cache'
    LANGUAGE C;

CREATE OR REPLACE FUNCTION prune_cache()
    RETURNS void
    EXECUTE ON ALL SEGMENTS
    AS 'MODULE_PATHNAME', 'prune_cache'
    LANGUAGE C;
