--
-- a regress test use pg_regress for vector_operator
--
-- start_matchignore
-- m/^WARNING:/
-- end_matchignore
CREATE EXTENSION vector_recall;

SELECT array_agg(id) AS ids,
    array_1d_extend(vector) AS vectors
FROM(
        SELECT id,
            (
                SELECT array_agg(gs::REAL) AS vector
                FROM generate_series(id, id + 9) AS gs
            )::REAL [] AS vector
        FROM generate_series(0, 99, 10) AS id
    ) AS tmp;

SELECT faiss_index_create(10, 'IDMap,HNSW32,Flat', 1);

SELECT faiss_index_train(
        faiss_index_create(10, 'IVF16,PQ5', 1),
        array_agg(gs::REAL),
        10
    )
FROM generate_series(1, 256 * 10) AS gs;

SELECT faiss_index_add(
        faiss_index_create(10, 'IDMap,HNSW32,Flat', 1),
        ARRAY [0,1,2,3,4,5,6,7,8,9],
        10,
        ARRAY [1]
    );

SELECT faiss_index_set_runtime_parameters(
        faiss_index_create(10, 'IDMap,HNSW32,Flat', 1),
        'efSearch=80'
    );

SELECT faiss_index_reset(
        faiss_index_add(
            faiss_index_create(10, 'IDMap,HNSW32,Flat', 1),
            ARRAY [0,1,2,3,4,5,6,7,8,9],
            10,
            ARRAY [1]
        )
    );

CREATE TABLE vector_queried (id BIGINT, vector REAL []) DISTRIBUTED BY (id);

INSERT INTO vector_queried
SELECT id,
    (
        SELECT array_agg(CAST(gs AS REAL)) AS vector
        FROM generate_series(id, id + 9) AS gs
    )
FROM generate_series(0, 99999, 10) AS id;

CREATE TABLE vector_query (id BIGINT, vector REAL []) DISTRIBUTED BY (id);

INSERT INTO vector_query
SELECT id,
    (
        SELECT array_agg(CAST(gs AS REAL) + 0.5) AS vector
        FROM generate_series(id, id + 9) AS gs
    )
FROM generate_series(0, 99999, 10005) AS id;

SELECT (m).query_idx,
    unnest((m).vector_idxs) AS vector_idx,
    unnest((m).distances) AS distance
FROM (
        SELECT faiss_index_search(
                index_table.faiss_index,
                queries.vectors,
                10,
                5,
                queries.ids,
                FALSE
            ) AS m
        FROM (
                SELECT create_index_agg(
                        vector,
                        'IDMap,HNSW32,Flat',
                        id,
                        1,
                        'efSearch=80'
                    ) AS faiss_index
                FROM vector_queried
            ) AS index_table,
            (
                SELECT array_agg(id) AS ids,
                    array_1d_extend(vector) AS vectors
                FROM vector_query
            ) AS queries
    ) AS foo
ORDER BY query_idx,
    distance;

SELECT (m).*
FROM (
        SELECT faiss_index_search(
                faiss_index_add(
                    faiss_index_create(10, 'IDMap,HNSW32,Flat'),
                    querieds.vectors,
                    10,
                    querieds.ids
                ),
                queries.vectors,
                10,
                5,
                queries.ids,
                TRUE
            ) AS m
        FROM (
                SELECT array_agg(id) AS ids,
                    array_1d_extend(vector) AS vectors
                FROM vector_queried
            ) AS querieds,
            (
                SELECT array_agg(id) AS ids,
                    array_1d_extend(vector) AS vectors
                FROM vector_query
            ) AS queries
    ) AS foo
ORDER BY query_idx;

SELECT (m).*
FROM (
        SELECT faiss_index_range_search(
                index_table.faiss_index,
                queries.vectors,
                10,
                100::REAL,
                queries.ids,
                FALSE
            ) AS m
        FROM (
                SELECT create_index_agg(
                        vector,
                        'IVF32,Flat',
                        id,
                        1,
                        NULL
                    ) AS faiss_index
                FROM vector_queried
            ) AS index_table,
            (
                SELECT array_agg(id) AS ids,
                    array_1d_extend(vector) AS vectors
                FROM vector_query
            ) AS queries
    ) AS foo
ORDER BY query_idx;

SELECT (m).*
FROM (
        SELECT (topk_merge(idxs, distance, 5)) AS m
        FROM (
                SELECT (
                        SELECT array_agg(CAST(gs AS BIGINT)) AS idxs
                        FROM generate_series(id, id + 30, 7) AS gs
                    ),
                    (
                        SELECT array_agg(CAST(gs + 100 AS REAL)) AS distance
                        FROM generate_series(id, id + 30, 7) AS gs
                    )
                FROM generate_series(0, 9, 3) AS id
            ) AS tmp_table
    ) AS foo;

CREATE TABLE index_table AS (
    SELECT sharding_id,
        md5(faiss_index) AS k,
        faiss_index
    FROM (
            SELECT CAST(id % 30 AS INT) AS sharding_id,
                create_index_agg(
                    vector,
                    'IVF1,Flat',
                    id,
                    1,
                    NULL
                ) AS faiss_index
            FROM vector_queried
            GROUP BY sharding_id
        ) AS foo
) DISTRIBUTED BY (sharding_id);

SELECT (m).*
FROM (
        SELECT faiss_index_search(
                faiss_index,
                ARRAY [0.5,1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5],
                10,
                5,
                NULL,
                TRUE,
                k
            ) AS m
        FROM index_table
        WHERE sharding_id = 0
    ) AS foo;

SELECT (m).*
FROM (
        SELECT faiss_index_range_search(
                faiss_index,
                ARRAY [0.5,1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5],
                10,
                100::REAL,
                NULL,
                TRUE,
                k
            ) AS m
        FROM index_table
        WHERE sharding_id = 0
    ) AS foo;

SELECT query_idx,
    idx,
    distance,
    ranking
FROM (
        SELECT (m).query_idx,
            topk_merge((m).vector_idxs, (m).distances, 5) AS t
        FROM (
                SELECT faiss_index_search(
                        index_table.faiss_index,
                        queries.vectors,
                        10,
                        5,
                        queries.ids,
                        FALSE
                    ) AS m
                FROM index_table,
                    (
                        SELECT array_agg(id) AS ids,
                            array_1d_extend(vector) AS vectors
                        FROM vector_query
                    ) AS queries
            ) local_topk_table
        GROUP BY (m).query_idx
    ) AS global_topk_table,
    LATERAL ROWS
FROM (unnest((t).idxs), unnest((t).distances)) WITH ORDINALITY ALIAS (idx, distance, ranking)
ORDER BY query_idx,
    distance;

DROP TABLE vector_queried;

DROP TABLE vector_query;

DROP TABLE index_table;

DROP EXTENSION vector_recall;