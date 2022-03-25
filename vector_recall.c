#include <string.h>

#include "postgres.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"

#include "cache/cache_c.h"

#include "Index_c.h"
#include "error_c.h"
#include "index_io_c.h"
#include "index_factory_c.h"
#include "AutoTune_c.h"
#include "impl/AuxIndexStructures_c.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define CHECK(condition) ereportif(!(condition), ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: (" CppAsString(condition) ") FailedCheck", __func__)))
#define FAISS_CHECK(C) ereportif((C), ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: (" CppAsString(C) ")'s faiss error: %s", __func__, faiss_get_last_error())))

#define ARRNELEMS(x) ArrayGetNItems(ARR_NDIM(x), ARR_DIMS(x))

typedef struct array_1d_extend_state
{
    uint8 *data;
    uint nbytes;
    uint32 elemnum;
    Oid element_type;
} array_1d_extend_state;

/**
 * faiss_search_result
 * created during SRF_IS_FIRSTCALL(), used each time of SRF CALL
 */
typedef struct faiss_search_result
{
    uint32 dim; // the length of query_vector
    union
    {
        uint32 topk;   // the input value of k
        float4 radius; // the input value of radius. for faiss range search
    };
    float4 *query_vectors; // the data of query_vectors. float array [query_vectors_num * dim]
    int64 *query_idxs;     // the id of query_vectors. int64 array [query_idxs_num]

    float4 *distances; // the distances result of faiss search.
    int64 *idxs;       // the id result of faiss search.

    size_t *lims;                                      //  for faiss range search
    FaissRangeSearchResult *faiss_range_search_result; //  for faiss range search
} faiss_search_result;

typedef struct create_index_state
{
    uint32 dim;
    uint32 size;
    uint32 capacity;
    FaissMetricType metric_type;
    float4 *data;
    idx_t *idxs;
    char description[256];
    char runtime_parameters[1000];
} create_index_state;

typedef struct topk_merge_state
{
    idx_t *idxs;
    float4 *distance;
    uint32 *lims;
    uint32 batch_num;
    uint32 topk;
} topk_merge_state;

typedef struct heap_buf
{
    float distance;
    uint32 batch_id;
} heap_buf;

void swap_buf(heap_buf *hb1, heap_buf *hb2);
void shiftdown(heap_buf *heap, uint32 start, uint32 end);
void build_min_heap(heap_buf **heap, float *distance, int64 *idxs, uint32 *batches_size, uint32 batch_num);
void heap_topk(heap_buf *heap, float *distance, int64 *idxs, Datum **result_distance, Datum **result_idxs, uint32 *batches_size, uint32 batch_num, uint32 topk);

bytea *faissindex2bytea(FaissIndex *fi);
FaissIndex *bytea2faissindex(const bytea *index_bytea);

cache_t *get_cache(size_t capacity);
void cache_item_deleter(const char *key, size_t keylen, void *value);

// it's too late to set env OMP_WAIT_POLICY
/*
void _PG_init(void);
void _PG_init(void)
{
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: old env OMP_WAIT_POLICY: %s", __func__, getenv("OMP_WAIT_POLICY"))));
    if (setenv("OMP_WAIT_POLICY", "PASSIVE", 1) != 0)
    {
        ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: faild to set env OMP_WAIT_POLICY.", __func__)));
    }
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: now env OMP_WAIT_POLICY: %s", __func__, getenv("OMP_WAIT_POLICY"))));
}
*/

PG_FUNCTION_INFO_V1(array_1d_extend_transfn);
Datum array_1d_extend_transfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    CHECK(!PG_ARGISNULL(1));
    ArrayType *in_arr = PG_GETARG_ARRAYTYPE_P(1);
    array_1d_extend_state *internal_state = NULL;

    MemoryContext old_context = MemoryContextSwitchTo(agg_context);
    if (unlikely(PG_ARGISNULL(0)))
    {
        const int elemnum = ARRNELEMS(in_arr);
        const int ndatabytes = ARR_SIZE(in_arr) - ARR_DATA_OFFSET(in_arr);
        internal_state = (array_1d_extend_state *)palloc0(sizeof(array_1d_extend_state));
        internal_state->elemnum = elemnum;
        internal_state->element_type = ARR_ELEMTYPE(in_arr);
        internal_state->nbytes = ndatabytes;
        internal_state->data = (uint8 *)palloc(ndatabytes);
        memcpy(internal_state->data, ARR_DATA_PTR(in_arr), ndatabytes);
    }
    else
    {
        internal_state = (array_1d_extend_state *)PG_GETARG_POINTER(0);
        if (internal_state->element_type != ARR_ELEMTYPE(in_arr) || ARR_NDIM(in_arr) != 1)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: The input array has wrong attribute", __func__)));

        const int elemnum = ARRNELEMS(in_arr);
        const int ndatabytes = ARR_SIZE(in_arr) - ARR_DATA_OFFSET(in_arr);
        internal_state->elemnum += elemnum;
        internal_state->data = (uint8 *)repalloc(internal_state->data, internal_state->nbytes + ndatabytes);
        memcpy(internal_state->data + internal_state->nbytes, ARR_DATA_PTR(in_arr), ndatabytes);
        internal_state->nbytes += ndatabytes;
    }

    MemoryContextSwitchTo(old_context);
    PG_RETURN_POINTER(internal_state);
}

PG_FUNCTION_INFO_V1(array_1d_extend_finalfn);
Datum array_1d_extend_finalfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    if (unlikely(PG_ARGISNULL(0)))
        PG_RETURN_NULL();

    array_1d_extend_state *internal_state = (array_1d_extend_state *)PG_GETARG_POINTER(0);
    int nbytes = internal_state->nbytes + ARR_OVERHEAD_NONULLS(1);

    ArrayType *result;
    result = (ArrayType *)palloc(nbytes);
    SET_VARSIZE(result, nbytes);
    result->ndim = 1;
    result->dataoffset = 0;
    result->elemtype = internal_state->element_type;
    *(ARR_DIMS(result)) = internal_state->elemnum;
    *(ARR_LBOUND(result)) = 1;
    memcpy(ARR_DATA_PTR(result), internal_state->data, internal_state->nbytes);

    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(faiss_index_create);
Datum faiss_index_create(PG_FUNCTION_ARGS)
{
    uint32 dim = PG_GETARG_UINT32(0);
    char *description = text_to_cstring(PG_GETARG_TEXT_P(1));
    FaissMetricType metric_type = PG_GETARG_INT32(2);
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: dim:%d, metric_type:%d", __func__, dim, metric_type)));

    FaissIndex *index = NULL;
    FAISS_CHECK(faiss_index_factory(&index, dim, description, metric_type));

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(faiss_index_train);
Datum faiss_index_train(PG_FUNCTION_ARGS)
{
    bytea *index_bytea = PG_GETARG_BYTEA_P(0);
    FaissIndex *index = bytea2faissindex(index_bytea);

    ArrayType *vectors_array = PG_GETARG_ARRAYTYPE_P(1);
    float4 *vectors = (float4 *)ARR_DATA_PTR(vectors_array);

    uint32 dim = PG_GETARG_UINT32(2);
    CHECK(dim == faiss_Index_d(index));

    int64 vectors_num = ARRNELEMS(vectors_array) / dim;
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: dim:%d, vectors_num:%ld", __func__, dim, vectors_num)));

    if (faiss_Index_is_trained(index))
        ereport(WARNING, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: faiss index can't or needn't train", __func__)));
    else
        FAISS_CHECK(faiss_Index_train(index, ARRNELEMS(vectors_array) / dim, vectors));

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(faiss_index_add);
Datum faiss_index_add(PG_FUNCTION_ARGS)
{
    CHECK(!PG_ARGISNULL(0));
    bytea *index_bytea = PG_GETARG_BYTEA_P(0);
    FaissIndex *index = bytea2faissindex(index_bytea);

    CHECK(!PG_ARGISNULL(1));
    ArrayType *vectors_array = PG_GETARG_ARRAYTYPE_P(1);
    float4 *vectors = (float4 *)ARR_DATA_PTR(vectors_array);

    CHECK(!PG_ARGISNULL(2));
    uint32 dim = PG_GETARG_UINT32(2);
    CHECK(dim == faiss_Index_d(index));

    int64 vectors_num = ARRNELEMS(vectors_array) / dim;
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: vectors_num:%ld", __func__, vectors_num)));

    if (unlikely(PG_ARGISNULL(3)))
    {
        FAISS_CHECK(faiss_Index_add(index, vectors_num, vectors)); // add vectors to the index
    }
    else
    {
        ArrayType *idxs_array = PG_GETARG_ARRAYTYPE_P(3);
        CHECK(ARRNELEMS(idxs_array) == vectors_num);

        int64 *idxs = (int64 *)ARR_DATA_PTR(idxs_array);
        FAISS_CHECK(faiss_Index_add_with_ids(index, vectors_num, vectors, (idx_t *)idxs)); // add vectors to the index
    }

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(faiss_index_set_runtime_parameters);
Datum faiss_index_set_runtime_parameters(PG_FUNCTION_ARGS)
{
    bytea *index_bytea = PG_GETARG_BYTEA_P(0);
    FaissIndex *index = bytea2faissindex(index_bytea);

    char *runtime_parameters = text_to_cstring(PG_GETARG_TEXT_P(1));
    ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: input runtime_parameters: %s", __func__, runtime_parameters)));

    FaissParameterSpace *parameter_space = NULL;
    FAISS_CHECK(faiss_ParameterSpace_new(&parameter_space));
    FAISS_CHECK(faiss_ParameterSpace_set_index_parameters(parameter_space, index, runtime_parameters));
    faiss_ParameterSpace_free(parameter_space);

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(faiss_index_reset);
Datum faiss_index_reset(PG_FUNCTION_ARGS)
{
    bytea *index_bytea = PG_GETARG_BYTEA_P(0);
    FaissIndex *index = bytea2faissindex(index_bytea);

    FAISS_CHECK(faiss_Index_reset(index));

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(create_index_transfn);
Datum create_index_transfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context, old_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    CHECK(!PG_ARGISNULL(1));
    ArrayType *vector_array = PG_GETARG_ARRAYTYPE_P(1);
    create_index_state *internal_state = NULL;
    old_context = MemoryContextSwitchTo(agg_context);

    if (unlikely(PG_ARGISNULL(0)))
    {
        internal_state = (create_index_state *)palloc0(sizeof(create_index_state));
        internal_state->size = 0;
        internal_state->capacity = 16;
        internal_state->dim = ARRNELEMS(vector_array);
        internal_state->data = palloc(internal_state->capacity * internal_state->dim * sizeof(internal_state->data[0]));

        CHECK(!PG_ARGISNULL(2));
        char *description = text_to_cstring(PG_GETARG_TEXT_P(2));
        strncpy(internal_state->description, description, sizeof(internal_state->description) / sizeof(internal_state->description[0]));

        if (likely(!PG_ARGISNULL(3)))
            internal_state->idxs = palloc(internal_state->capacity * sizeof(internal_state->idxs[0]));

        if (!PG_ARGISNULL(4))
            internal_state->metric_type = PG_GETARG_INT32(4);
        else
            internal_state->metric_type = METRIC_L2;

        if (!PG_ARGISNULL(5))
        {
            char *runtime_parameters = text_to_cstring(PG_GETARG_TEXT_P(5));
            strncpy(internal_state->runtime_parameters, runtime_parameters, sizeof(internal_state->runtime_parameters) / sizeof(internal_state->runtime_parameters[0]));
            ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: input runtime_parameters: %s", __func__, runtime_parameters)));
        }
    }
    else
    {
        internal_state = (create_index_state *)PG_GETARG_POINTER(0);
        CHECK(internal_state->dim == ARRNELEMS(vector_array));
    }

    if (internal_state->size == internal_state->capacity)
    {
        internal_state->capacity += (internal_state->capacity / 2);
        internal_state->data = repalloc(internal_state->data, internal_state->capacity * internal_state->dim * sizeof(internal_state->data[0]));
        if (likely(!PG_ARGISNULL(3)))
            internal_state->idxs = repalloc(internal_state->idxs, internal_state->capacity * sizeof(internal_state->idxs[0]));
    }

    memcpy(internal_state->data + internal_state->size * internal_state->dim, ARR_DATA_PTR(vector_array), internal_state->dim * sizeof(internal_state->data[0]));
    if (likely(!PG_ARGISNULL(3)))
        internal_state->idxs[internal_state->size] = PG_GETARG_INT64(3);

    internal_state->size += 1;

    MemoryContextSwitchTo(old_context);
    PG_RETURN_POINTER(internal_state);
}

PG_FUNCTION_INFO_V1(create_index_finalfn);
Datum create_index_finalfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    if (unlikely(PG_ARGISNULL(0)))
        PG_RETURN_NULL();

    create_index_state *internal_state = (create_index_state *)PG_GETARG_POINTER(0);

    FaissIndex *index = NULL;
    FAISS_CHECK(faiss_index_factory(&index, internal_state->dim, internal_state->description, METRIC_L2));

    if (strnlen(internal_state->runtime_parameters, sizeof(internal_state->runtime_parameters) / sizeof(internal_state->runtime_parameters[0])))
    {
        char *runtime_parameters = internal_state->runtime_parameters;
        ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: input runtime_parameters: %s", __func__, runtime_parameters)));
        FaissParameterSpace *parameter_space = NULL;
        FAISS_CHECK(faiss_ParameterSpace_new(&parameter_space));
        FAISS_CHECK(faiss_ParameterSpace_set_index_parameters(parameter_space, index, runtime_parameters));
        faiss_ParameterSpace_free(parameter_space);
    }

    if (faiss_Index_is_trained(index))
        ereport(LOG, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: faiss index can't or needn't train", __func__)));
    else
        FAISS_CHECK(faiss_Index_train(index, internal_state->size, internal_state->data));

    if (internal_state->idxs)
        FAISS_CHECK(faiss_Index_add_with_ids(index, internal_state->size, internal_state->data, internal_state->idxs));
    else
        FAISS_CHECK(faiss_Index_add(index, internal_state->size, (float *)(internal_state->data)));

    PG_RETURN_BYTEA_P(faissindex2bytea(index));
}

PG_FUNCTION_INFO_V1(faiss_index_search);
Datum faiss_index_search(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;
    TupleDesc tupdesc;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;

        // SRF init and get tuple_desc
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("function returning record called in context that cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        FaissIndex *faiss_index = NULL;

        cache_t *cache = get_cache(0);
        handle_t *handle = NULL;
        if (!PG_ARGISNULL(6))
        {
            char *key = text_to_cstring(PG_GETARG_TEXT_P(6));
            size_t keylen = strlen(key);
            handle = cache_lookup(cache, key, keylen);
            if (handle)
            {
                faiss_index = cache_value(cache, handle);
                ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: cache hit: faiss_index:%p handle:%p", __func__, faiss_index, handle)));
            }
            else
            {
                CHECK(!PG_ARGISNULL(0));
                bytea *index_bytea = PG_GETARG_BYTEA_P(0);
                faiss_index = bytea2faissindex(index_bytea);
                handle = cache_insert(cache, key, keylen, faiss_index, VARSIZE(index_bytea), cache_item_deleter);
                ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: cache miss: faiss_index:%p handle:%p", __func__, faiss_index, handle)));
            }
        }
        else
        {
            CHECK(!PG_ARGISNULL(0));
            bytea *index_bytea = PG_GETARG_BYTEA_P(0);
            faiss_index = bytea2faissindex(index_bytea);
        }

        CHECK(!PG_ARGISNULL(1));
        ArrayType *query_vectors_array = PG_GETARG_ARRAYTYPE_P(1);
        CHECK(!PG_ARGISNULL(2));
        uint32 dim = PG_GETARG_UINT32(2);
        CHECK(!PG_ARGISNULL(3));
        uint32 topk = PG_GETARG_UINT32(3);
        CHECK(dim == faiss_Index_d(faiss_index));

        // get query_vectors data and infomation
        float4 *query_vectors = (float4 *)ARR_DATA_PTR(query_vectors_array);
        uint32 query_vectors_num = ARRNELEMS(query_vectors_array) / dim;
        int64 *query_idxs = NULL;
        uint32 query_idxs_num = 0;

        // if provide query_idx, then output with query_idx
        if (!PG_ARGISNULL(4))
        {
            ArrayType *query_idxs_array = PG_GETARG_ARRAYTYPE_P(4);
            query_idxs = (int64 *)ARR_DATA_PTR(query_idxs_array);
            query_idxs_num = ARRNELEMS(query_idxs_array);
            CHECK(query_idxs_num == query_vectors_num);
        }

        // get the result of faiss search, construct the faiss_search_result.
        faiss_search_result *search_result = (faiss_search_result *)palloc0(sizeof(faiss_search_result));
        search_result->dim = dim;
        search_result->topk = topk;
        search_result->query_vectors = (!PG_ARGISNULL(5) && PG_GETARG_BOOL(5)) ? query_vectors : NULL;
        search_result->query_idxs = query_idxs;
        search_result->distances = palloc(topk * query_vectors_num * sizeof(float4));
        search_result->idxs = palloc(topk * query_vectors_num * sizeof(int64));

        FAISS_CHECK(faiss_Index_search(faiss_index, query_vectors_num, query_vectors, topk, search_result->distances, search_result->idxs));
        if (!PG_ARGISNULL(6))
            cache_release(cache, handle);
        else
            faiss_Index_free(faiss_index);

        funcctx->user_fctx = search_result;
        // set the number of SRF CALL, also as the output rows number.
        funcctx->max_calls = query_vectors_num;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if (call_cntr < max_calls)
    {
        Datum result;
        HeapTuple tuple;

        faiss_search_result *search_result = funcctx->user_fctx;
        uint32 dim = search_result->dim;
        uint32 topk = search_result->topk;
        uint32 query_idx = call_cntr;

        Datum values[4];
        bool nulls[4];

        int16 elmlen_out;
        bool elmbyval_out;
        char elmalign_out;

        if (search_result->query_vectors)
        {
            Datum query_vector[dim];
            for (uint32 i = 0; i < dim; ++i)
                query_vector[i] = Float4GetDatum(search_result->query_vectors[query_idx * dim + i]);
            get_typlenbyvalalign(FLOAT4OID, &elmlen_out, &elmbyval_out, &elmalign_out);
            ArrayType *array = construct_array(query_vector, dim, FLOAT4OID, elmlen_out, elmbyval_out, elmalign_out);

            values[0] = PointerGetDatum(array);
            nulls[0] = false;
        }
        else
        {
            nulls[0] = true;
        }

        if (search_result->query_idxs)
        {
            values[1] = Int64GetDatum(search_result->query_idxs[query_idx]);
            nulls[1] = false;
        }
        else
        {
            nulls[1] = true;
        }

        ArrayType *dis_array, *idx_array;
        Datum dis_vector[topk];
        Datum idx_vector[topk];

        uint32 cnt = 0;
        for (; cnt < topk && search_result->idxs[query_idx * topk + cnt] != -1; ++cnt)
        {
            idx_vector[cnt] = Int64GetDatum(search_result->idxs[query_idx * topk + cnt]);
            dis_vector[cnt] = Float4GetDatum(search_result->distances[query_idx * topk + cnt]);
        }

        get_typlenbyvalalign(INT8OID, &elmlen_out, &elmbyval_out, &elmalign_out);
        idx_array = construct_array(idx_vector, cnt, INT8OID, elmlen_out, elmbyval_out, elmalign_out);
        values[2] = PointerGetDatum(idx_array);
        nulls[2] = false;

        get_typlenbyvalalign(FLOAT4OID, &elmlen_out, &elmbyval_out, &elmalign_out);
        dis_array = construct_array(dis_vector, cnt, FLOAT4OID, elmlen_out, elmbyval_out, elmalign_out);
        values[3] = PointerGetDatum(dis_array);
        nulls[3] = false;

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        result = HeapTupleGetDatum(tuple);

        SRF_RETURN_NEXT(funcctx, result);
    }
    else
    {
        SRF_RETURN_DONE(funcctx);
    }
}

PG_FUNCTION_INFO_V1(faiss_index_range_search);
Datum faiss_index_range_search(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;
    TupleDesc tupdesc;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;

        // SRF init and get tuple_desc
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("function returning record called in context that cannot accept type record")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        FaissIndex *faiss_index = NULL;

        cache_t *cache = get_cache(0);
        handle_t *handle = NULL;
        if (!PG_ARGISNULL(6))
        {
            char *key = text_to_cstring(PG_GETARG_TEXT_P(6));
            size_t keylen = strlen(key);
            handle = cache_lookup(cache, key, keylen);
            if (handle)
            {
                faiss_index = cache_value(cache, handle);
                ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: cache hit: faiss_index:%p handle:%p", __func__, faiss_index, handle)));
            }
            else
            {
                CHECK(!PG_ARGISNULL(0));
                bytea *index_bytea = PG_GETARG_BYTEA_P(0);
                faiss_index = bytea2faissindex(index_bytea);
                handle = cache_insert(cache, key, keylen, faiss_index, VARSIZE(index_bytea), cache_item_deleter);
                ereport(DEBUG2, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: cache miss: faiss_index:%p handle:%p", __func__, faiss_index, handle)));
            }
        }
        else
        {
            CHECK(!PG_ARGISNULL(0));
            bytea *index_bytea = PG_GETARG_BYTEA_P(0);
            faiss_index = bytea2faissindex(index_bytea);
        }

        CHECK(!PG_ARGISNULL(1));
        ArrayType *query_vectors_array = PG_GETARG_ARRAYTYPE_P(1);
        CHECK(!PG_ARGISNULL(2));
        uint32 dim = PG_GETARG_UINT32(2);
        CHECK(!PG_ARGISNULL(3));
        float4 radius = PG_GETARG_FLOAT4(3);
        CHECK(dim == faiss_Index_d(faiss_index));

        // get query_vectors data and infomation
        float4 *query_vectors = (float4 *)ARR_DATA_PTR(query_vectors_array);
        uint32 query_vectors_num = ARRNELEMS(query_vectors_array) / dim;
        int64 *query_idxs = NULL;
        uint32 query_idxs_num = 0;

        // if provide query_idx, then output with query_idx
        if (!PG_ARGISNULL(4))
        {
            ArrayType *query_idxs_array = PG_GETARG_ARRAYTYPE_P(4);
            query_idxs = (int64 *)ARR_DATA_PTR(query_idxs_array);
            query_idxs_num = ARRNELEMS(query_idxs_array);
            CHECK(query_idxs_num == query_vectors_num);
        }

        faiss_search_result *search_result = (faiss_search_result *)palloc0(sizeof(faiss_search_result));
        search_result->dim = dim;
        search_result->radius = radius;
        search_result->query_vectors = (!PG_ARGISNULL(5) && PG_GETARG_BOOL(5)) ? query_vectors : NULL;
        search_result->query_idxs = query_idxs;

        FAISS_CHECK(faiss_RangeSearchResult_new(&(search_result->faiss_range_search_result), query_vectors_num));
        FAISS_CHECK(faiss_Index_range_search(faiss_index, query_vectors_num, query_vectors, radius, search_result->faiss_range_search_result));
        if (!PG_ARGISNULL(6))
            cache_release(cache, handle);
        else
            faiss_Index_free(faiss_index);

        faiss_RangeSearchResult_lims(search_result->faiss_range_search_result, &(search_result->lims));
        faiss_RangeSearchResult_labels(search_result->faiss_range_search_result, &(search_result->idxs), &(search_result->distances));

        funcctx->user_fctx = search_result;
        // set the number of SRF CALL, also as the output rows number.
        funcctx->max_calls = query_vectors_num;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;
    faiss_search_result *search_result = funcctx->user_fctx;

    if (call_cntr < max_calls)
    {
        Datum result;
        HeapTuple tuple;

        uint32 dim = search_result->dim;
        uint32 query_idx = call_cntr;

        Datum values[4];
        bool nulls[4];

        int16 elmlen_out;
        bool elmbyval_out;
        char elmalign_out;

        if (search_result->query_vectors)
        {
            Datum query_vector[dim];
            for (uint32 i = 0; i < dim; ++i)
                query_vector[i] = Float4GetDatum(search_result->query_vectors[query_idx * dim + i]);
            get_typlenbyvalalign(FLOAT4OID, &elmlen_out, &elmbyval_out, &elmalign_out);
            ArrayType *array = construct_array(query_vector, dim, FLOAT4OID, elmlen_out, elmbyval_out, elmalign_out);

            values[0] = PointerGetDatum(array);
            nulls[0] = false;
        }
        else
        {
            nulls[0] = true;
        }

        if (search_result->query_idxs)
        {
            values[1] = Int64GetDatum(search_result->query_idxs[query_idx]);
            nulls[1] = false;
        }
        else
        {
            nulls[1] = true;
        }

        // result for query i is labels[lims[i]:lims[i+1]]
        size_t ofs = search_result->lims[call_cntr], lim = search_result->lims[call_cntr + 1] - search_result->lims[call_cntr];
        Datum dis_vector[lim];
        Datum idx_vector[lim];

        for (size_t i = 0; i < lim; ++i)
        {
            idx_vector[i] = Int64GetDatum(search_result->idxs[ofs + i]);
            dis_vector[i] = Float4GetDatum(search_result->distances[ofs + i]);
        }

        get_typlenbyvalalign(INT8OID, &elmlen_out, &elmbyval_out, &elmalign_out);
        ArrayType *idx_array = construct_array(idx_vector, lim, INT8OID, elmlen_out, elmbyval_out, elmalign_out);
        values[2] = PointerGetDatum(idx_array);
        nulls[2] = false;

        get_typlenbyvalalign(FLOAT4OID, &elmlen_out, &elmbyval_out, &elmalign_out);
        ArrayType *dis_array = construct_array(dis_vector, lim, FLOAT4OID, elmlen_out, elmbyval_out, elmalign_out);
        values[3] = PointerGetDatum(dis_array);
        nulls[3] = false;

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        result = HeapTupleGetDatum(tuple);

        SRF_RETURN_NEXT(funcctx, result);
    }
    else
    {
        faiss_RangeSearchResult_free(search_result->faiss_range_search_result);
        SRF_RETURN_DONE(funcctx);
    }
}

PG_FUNCTION_INFO_V1(topk_merge_transfn);
Datum topk_merge_transfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;
    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    CHECK(!PG_ARGISNULL(1));
    ArrayType *idxs_arr = PG_GETARG_ARRAYTYPE_P(1);
    CHECK(!PG_ARGISNULL(2));
    ArrayType *distance_arr = PG_GETARG_ARRAYTYPE_P(2);
    CHECK(!PG_ARGISNULL(3));
    uint32 topk = PG_GETARG_UINT32(3);

    const uint32 elemnum = ARRNELEMS(idxs_arr);
    CHECK(elemnum == ARRNELEMS(distance_arr));

    if (elemnum == 0 || topk == 0)
    {
        if (unlikely(PG_ARGISNULL(0)))
            PG_RETURN_NULL();
        else
            PG_RETURN_POINTER(PG_GETARG_POINTER(0));
    }

    topk_merge_state *internal_state = NULL;
    MemoryContext old_context = MemoryContextSwitchTo(agg_context);
    if (unlikely(PG_ARGISNULL(0)))
    {
        internal_state = (topk_merge_state *)palloc0(sizeof(topk_merge_state));
        internal_state->topk = topk;
        internal_state->batch_num = 1;
        internal_state->distance = palloc(elemnum * sizeof(internal_state->distance[0]));
        internal_state->idxs = palloc(elemnum * sizeof(internal_state->idxs[0]));
        internal_state->lims = palloc(2 * sizeof(internal_state->lims[0]));
        internal_state->lims[0] = 0;
        internal_state->lims[1] = elemnum;
        memcpy(internal_state->idxs, ARR_DATA_PTR(idxs_arr), elemnum * sizeof(internal_state->idxs[0]));
        memcpy(internal_state->distance, ARR_DATA_PTR(distance_arr), elemnum * sizeof(internal_state->distance[0]));
    }
    else
    {
        internal_state = (topk_merge_state *)PG_GETARG_POINTER(0);
        CHECK(internal_state->topk == topk);

        uint32 b = ++(internal_state->batch_num);
        internal_state->lims = repalloc(internal_state->lims, (b + 1) * sizeof(internal_state->lims[0]));
        internal_state->lims[b] = internal_state->lims[b - 1] + elemnum;
        internal_state->idxs = repalloc(internal_state->idxs, internal_state->lims[b] * sizeof(internal_state->idxs[0]));
        internal_state->distance = repalloc(internal_state->distance, internal_state->lims[b] * sizeof(internal_state->distance[0]));
        memcpy(internal_state->idxs + internal_state->lims[b - 1], ARR_DATA_PTR(idxs_arr), elemnum * sizeof(internal_state->idxs[0]));
        memcpy(internal_state->distance + internal_state->lims[b - 1], ARR_DATA_PTR(distance_arr), elemnum * sizeof(internal_state->distance[0]));
    }

    MemoryContextSwitchTo(old_context);
    PG_RETURN_POINTER(internal_state);
}

PG_FUNCTION_INFO_V1(topk_merge_finalfn);
Datum topk_merge_finalfn(PG_FUNCTION_ARGS)
{
    MemoryContext agg_context;

    if (!AggCheckCallContext(fcinfo, &agg_context))
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: aggregate function called in non-aggregate context", __func__)));

    if (unlikely(PG_ARGISNULL(0)))
        PG_RETURN_NULL();

    TupleDesc tuple_desc;
    if (get_call_result_type(fcinfo, NULL, &tuple_desc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("function returning record called in context that cannot accept type record")));
    tuple_desc = BlessTupleDesc(tuple_desc);

    topk_merge_state *internal_state = (topk_merge_state *)PG_GETARG_POINTER(0);
    uint32 batch_num = internal_state->batch_num;
    uint32 *lims = internal_state->lims;
    uint32 topk = ((internal_state->topk <= lims[batch_num]) ? internal_state->topk : lims[batch_num]);
    float *distance = internal_state->distance;
    int64 *idxs = internal_state->idxs;

    heap_buf *heap = NULL;
    Datum *result_distance, *result_idxs;

    build_min_heap(&heap, distance, idxs, lims, batch_num);

    heap_topk(heap, distance, idxs, &result_distance, &result_idxs, lims, batch_num, topk);

    Datum values[2];
    bool nulls[2];

    int16 elmlen_out;
    bool elmbyval_out;
    char elmalign_out;

    get_typlenbyvalalign(INT8OID, &elmlen_out, &elmbyval_out, &elmalign_out);
    ArrayType *idxs_arr = construct_array(result_idxs, topk, INT8OID, elmlen_out, elmbyval_out, elmalign_out);
    values[0] = PointerGetDatum(idxs_arr);
    nulls[0] = false;

    get_typlenbyvalalign(FLOAT4OID, &elmlen_out, &elmbyval_out, &elmalign_out);
    ArrayType *distance_arr = construct_array(result_distance, topk, FLOAT4OID, elmlen_out, elmbyval_out, elmalign_out);
    values[1] = PointerGetDatum(distance_arr);
    nulls[1] = false;

    HeapTuple tuple = heap_form_tuple(tuple_desc, values, nulls);
    Datum result = HeapTupleGetDatum(tuple);

    PG_RETURN_DATUM(result);
}

bytea *faissindex2bytea(FaissIndex *faiss_index)
{
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *fp_write = open_memstream(&buf, &buf_size);
    if (fp_write == NULL)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: open_memstream failed!", __func__)));
    FAISS_CHECK(faiss_write_index(faiss_index, fp_write));
    fclose(fp_write);
    faiss_Index_free(faiss_index);

    uint32 var_size = (uint32)buf_size + VARHDRSZ;
    bytea *ret_bytea = palloc(var_size);
    SET_VARSIZE(ret_bytea, var_size);
    memcpy(VARDATA(ret_bytea), buf, buf_size);
    free(buf);

    return ret_bytea;
}

FaissIndex *bytea2faissindex(const bytea *index_bytea)
{
    FaissIndex *index = NULL;
    char *index_buf = (char *)VARDATA(index_bytea);
    size_t buf_size = (size_t)VARSIZE(index_bytea) - VARHDRSZ;
    FILE *fp_index = fmemopen(index_buf, buf_size, "r");
    if (fp_index == NULL)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("%s: fmemopen failed!", __func__)));

    FAISS_CHECK(faiss_read_index(fp_index, 2, &index));
    fclose(fp_index);

    return index;
}

void swap_buf(heap_buf *hb1, heap_buf *hb2)
{
    float dtmp = hb1->distance;
    uint32 btmp = hb1->batch_id;
    hb1->distance = hb2->distance;
    hb1->batch_id = hb2->batch_id;
    hb2->distance = dtmp;
    hb2->batch_id = btmp;
}

void shiftdown(heap_buf *heap, uint32 start, uint32 end)
{
    uint32 parent = start;
    uint32 smaller = parent;
    while (parent < end)
    {
        uint32 lchild = parent * 2 + 1;
        uint32 rchild = lchild + 1;
        if (lchild < end && heap[lchild].distance < heap[smaller].distance)
            smaller = lchild;
        if (rchild < end && heap[rchild].distance < heap[smaller].distance)
            smaller = rchild;
        if (smaller == parent)
            break;
        swap_buf(heap + smaller, heap + parent);
        parent = smaller;
    }
}

void build_min_heap(heap_buf **heap, float *distance, int64 *idxs, uint32 *batches_size, uint32 batch_num)
{
    *heap = palloc((batch_num) * sizeof(heap_buf));
    for (uint32 i = 0; i < batch_num; ++i)
    {
        (*heap)[i].distance = distance[batches_size[i]];
        (*heap)[i].batch_id = i;
    }

    for (int i = batch_num / 2 - 1; i >= 0; --i)
        shiftdown(*heap, i, batch_num);
}

void heap_topk(heap_buf *heap, float *distance, int64 *idxs, Datum **result_distance, Datum **result_idxs, uint32 *batches_size, uint32 batch_num, uint32 topk)
{
    *result_distance = palloc(topk * sizeof(Datum));
    *result_idxs = palloc(topk * sizeof(Datum));

    uint32 *batches_pos = palloc(batch_num * sizeof(uint32));
    for (uint32 i = 0; i < batch_num; ++i)
        batches_pos[i] = batches_size[i];

    (*result_distance)[0] = Float4GetDatum(heap[0].distance);
    (*result_idxs)[0] = Int64GetDatum(idxs[batches_pos[heap[0].batch_id]]);

    for (uint32 i = 1; i < topk; ++i)
    {
        batches_pos[heap[0].batch_id]++;
        if (batches_pos[heap[0].batch_id] == batches_size[heap[0].batch_id + 1])
        {
            --batch_num;
            heap[0].distance = heap[batch_num].distance;
            heap[0].batch_id = heap[batch_num].batch_id;
        }
        else
        {
            heap[0].distance = distance[batches_pos[heap[0].batch_id]];
        }
        shiftdown(heap, 0, batch_num);
        (*result_distance)[i] = Float4GetDatum(distance[batches_pos[heap[0].batch_id]]);
        (*result_idxs)[i] = Int64GetDatum(idxs[batches_pos[heap[0].batch_id]]);
    }
}

PG_FUNCTION_INFO_V1(reset_cache);
Datum reset_cache(PG_FUNCTION_ARGS)
{
    int64 capacity = PG_GETARG_INT64(0);
    cache_t *cache = get_cache(capacity);
    PG_RETURN_BOOL(!!cache);
}

PG_FUNCTION_INFO_V1(total_charge_cache);
Datum total_charge_cache(PG_FUNCTION_ARGS)
{
    cache_t *cache = get_cache(0);
    PG_RETURN_UINT64(cache ? cache_total_charge(cache) : 0);
}

PG_FUNCTION_INFO_V1(prune_cache);
Datum prune_cache(PG_FUNCTION_ARGS)
{
    cache_t *cache = get_cache(0);
    if (cache)
        cache_prune(cache);
    PG_RETURN_NULL();
}

cache_t *get_cache(size_t capacity)
{
    static cache_t *cache = NULL;

    if (capacity && cache)
    {
        ereport(WARNING, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: found cache(%p), now destroy it", __func__, cache)));
        cache_destroy(cache);
        cache = NULL;
    }

    if (!cache)
    {
        size_t cap = capacity ? capacity : 1 << 25;
        cache = cache_create_lru(cap);
        ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION), errmsg("%s: cache_create_lru(%zu)=%p", __func__, cap, cache)));
    }

    return cache;
}

void cache_item_deleter(const char *key, size_t keylen, void *value)
{
    faiss_Index_free((FaissIndex *)value);
}
