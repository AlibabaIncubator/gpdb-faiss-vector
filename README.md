# vector_recall
vector_recall extension for greenplum-db

[faiss](https://github.com/facebookresearch/faiss)是用于高效相似性检索和密集向量聚类的库。

[greenplum](https://github.com/greenplum-db/gpdb)是基于PostgreSQL用于分析的大规模并行处理架构的分布式数据库

本插件是将faiss集成到greenplum数据库中，以提供向量召回的能力。典型压测性能QPS约为4k+，RT约为25ms。特点如下：
* 无需改动greenplum和faiss的代码
* 使用数据库的bytea数据类型存放faiss index字节序列。
* 提供了创建，训练和检索faiss index等sql函数。
* 创建函数使用工厂模式，可以完美支持faiss各种index。
* 检索函数支持topk检索和阈值检索。
* 内置cache，可以缓存faiss index内存对象，节省数据库读取faiss index字节序列和将其反序列化为内存对象的耗时（实践发现，这两步是主要耗时环节）。
* 借助数据库自身能力，可支持标签+向量联合检索场景
* 提供*topk_merge*函数，优化数据分片场景下的检索结果合并环节


# cache
插件内有缓存faiss index内存对象的cache，移植自[leveldb](https://github.com/google/leveldb)。cache底层由哈希表和链表组合而成，基于LRU策略。该cache的缓存条目，其key为用户传入的*faiss_index_key*字符串，其value为由*faiss_index*字节序列反序列化得到的faiss index内存对象，其charge（费用，或称权重）为*faiss_index*字节序列大小。可用于加速*faiss_index_search*和*faiss_index_range_search*。

该cache具有如下特点：
* 能pin住缓存条目：当某个v正在被使用时，该条目不会被驱逐出cache
* 支持自定义value删除器：因为v是个对象，所以cache驱逐某个v时，不能直接用free，而是要调用相应的删除器（deleter），如对象析构函数
* 多线程安全

可通过如下函数管控：
* reset_cache
* total_charge_cache
* prune_cache

# 数据类型

## __vector_index_search_results 
用作检索函数（*faiss_index_search*和*faiss_index_range_search*）返回的数据类型

| 参数 | 含义|
| --- | --- |
|query_vector REAL[] |查询向量值|
|query_idx BIGINT|查询向量的ID|
|vector_idxs BIGINT[] |查询得到的K个向量的ID|
|distances REAL[] |查询得到的K（若实际检索到的向量数目不足topk的k，则K为实际向量个数）个向量的距离，递增排序，与*vector_idxs*一一对应|

## __topk_merge_result
用作*topk_merge*函数的返回类型

| 参数 | 含义|
| --- | --- |
|idxs BIGINT[] |聚合所有局部topk结果后得到的最终topk结果，长度取决于输入时的topk参数|
|distances REAL[] |聚合后得到的距离结果，递增排序，与idxs一一对应|

# 函数

## array_1d_extend
UDAF。将多个1维real数组聚合extend成一个大的real数组。faiss c api规定要输入一维float数组，即使是实际要输入多个向量。

| 参数 | 含义|
| --- | --- |
| real_array REAL[] | 原始的一维float数组 |

```sql
SELECT array_1d_extend(vector) AS vectors
FROM vector_table;
```

## faiss_index_create
UDF。对应于faiss的*faiss_index_factory*，用于创建faiss index。

| 参数 | 含义|
| --- | --- |
| dim INT|  原始向量的维度 |
| index_desc TEXT = 'IDMap,HNSW32,Flat' | faiss index的类型。工厂模式，输入字符串（可参考[The index factory](https://github.com/facebookresearch/faiss/wiki/The-index-factory)）即可创建相应类型的faiss index|
| metric_type INT = 1 | 度量类型，又称距离。可参考[MetricType.h](https://github.com/facebookresearch/faiss/blob/main/faiss/MetricType.h)|

```sql
SELECT faiss_index_create(10, 'IDMap,HNSW32,Flat', 1) AS faiss_index;
```

## faiss_index_train
UDF。对应于faiss的*faiss_Index_train*，根据输入的向量训练faiss index。

| 参数 | 含义|
| --- | --- |
|faiss_index BYTEA| 待训练处理的faiss index |
| vectors REAL[]| 由多个训练集原始向量聚合而成的向量 |
| dim INT | 原始向量的维度 |

```sql
SELECT faiss_index_train(
        faiss_index,
        array_1d_extend(vector),
        10
    )
FROM vector_table;
```

## faiss_index_add
UDF。对应于faiss的*faiss_Index_add*，将向量添加到faiss index里。

| 参数 | 含义|
| --- | --- |
|faiss_index BYTEA| 待添加向量的faiss index |
| vectors REAL[]| 由多个原始向量聚合而成的向量 |
| dim INT| 原始向量的维度  |
| vector_idxs BIGINT[] = NULL | 原始向量对应ID的数组（长度为原始向量个数）。如果该输入为*NULL*，则在执行search函数时返回的*vector_idxs*为按照add顺序自动累计的默认ID（从0开始）。 |

```sql
SELECT faiss_index_add(
        faiss_index,
        array_1d_extend(vector),
        10,
        array_agg(id)
    );
FROM vector_table;
```

## faiss_index_set_runtime_parameters
UDF。对应于faiss的*faiss_ParameterSpace_set_index_parameters* ，可用来设置faiss index运行时参数。其底层借助[The ParameterSpace object](https://github.com/facebookresearch/faiss/wiki/Index-IO,-cloning-and-hyper-parameter-tuning#the-parameterspace-object)进行运行时参数的设置。

| 参数 | 含义|
| --- | --- |
|faiss_index BYTEA| 待调整的faiss index|
| runtime_parameters TEXT| 字符串表示的具体配置项，可参阅[auto-tuning-the-runtime-parameters](https://github.com/facebookresearch/faiss/wiki/Index-IO,-cloning-and-hyper-parameter-tuning#auto-tuning-the-runtime-parameters)和[AutoTune.cpp](https://github.com/facebookresearch/faiss/blob/main/faiss/AutoTune.cpp)的```ParameterSpace::set_index_parameter( Index* index, const std::string& name, double val)```函数|

```sql
SELECT faiss_index_set_runtime_parameters(faiss_index, 'efSearch=80');
```

## faiss_index_reset
UDF。对应于faiss的*faiss_Index_reset*，可用来重置faiss index

| 参数 | 含义|
| --- | --- |
|faiss_index BYTEA | 待重置的faiss index |

```sql
SELECT faiss_index_reset(faiss_index);
```

## create_index_agg
UDAF。辅助函数，封装了create、set runtime parameters、train和add环节。

| 参数 | 含义|
| --- | --- |
|vector REAL[]| 原始待检索向量，参与train和add |
| index_desc TEXT| 同*faiss_index_create*的*index_desc*|
| vector_idx BIGINT| 原始待检索向量的ID|
|metric_type INT| 同*faiss_index_create*的*metric_type* |
| runtime_parameters TEXT| 同*faiss_index_set_runtime_parameters*的*runtime_parameters*|

```sql
SELECT create_index_agg(
        vector,
        'IDMap,HNSW32,Flat',
        id,
        1,
        'efSearch=80'
    ) AS faiss_index
FROM vector_table;
```

## faiss_index_search
UDTF。对应于faiss的*faiss_Index_search*，从faiss index中检索并返回距离topk的邻居向量们。

| 参数 | 含义|
| --- | --- |
| faiss_index BYTEA| 待检索的faiss index |
| query_vectors REAL[]| 由多个原始查询向量聚合而成的向量|
| dim INT| 原始查询向量的维度 |
| topk INT| 检索最近邻居的数目K |
| query_idxs BIGINT[] = NULL| 原始查询向量ID的数组，如果提不为NULL，则在输出结果中返回查询向量对应的ID |
| preserve_vector bool = TRUE| 返回结果中是否保留原始查询向量值 |
| faiss_index_key TEXT = NULL| 被用作内部cache缓存条目的key。如果为NULL，则不用cache，否则使用cache，并自动缓存|

简单起见，*faiss_index_key*参数可输入*faiss_index*的md5值

```sql
SELECT (m).*
FROM (
        SELECT faiss_index_search(
                index_table.faiss_index,
                queries.vectors,
                10,
                5,
                queries.ids,
                TRUE,
                index_table.faiss_index_key
            ) AS m
        FROM index_table, queries
    ) AS foo;
```

## faiss_index_range_search
UDTF。对应于faiss的*faiss_Index_range_search*，从faiss index中检索并返回距离小于阈值的向量们。

| 参数 | 含义|
| --- | --- |
|faiss_index BYTEA|  同*faiss_index_search*的*faiss_index*|
| query_vectors REAL[]|同*faiss_index_search*的*query_vectors* |
| dim INT| 同*faiss_index_search*的*dim*|
| radius REAL| 距离的阈值 |
| query_idxs BIGINT[] = NULL|同*faiss_index_search*的*query_idxs*|
| preserve_vector bool = TRUE|同*faiss_index_search*的*preserve_vector*|
| faiss_index_key TEXT = NULL|同*faiss_index_search*的*faiss_index_key*|

```sql
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
        FROM index_table, queries
    ) AS foo;
```

## topk_merge
UDAF。用于合并多个局部topk为一个全局topk。可用于处理*faiss_index_search*的输出。

使用场景：待检索向量集合分片，分别建立faiss index，使用*faiss_index_search*分别检索得到局部topk结果，然后使用该*topk_merge*函数汇总求得全局topk结果。

| 参数 | 含义|
| --- | --- |
| idxs BIGINT[]| 与*distance*相对应的向量ID的数组 |
| distance REAL[]| 检索得到的局部topk向量距离的数组，应是递增有序 |
| topk INT| 期望的全局topk向量的数目K |

```sql
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
        FROM index_table, queries
    ) local_topk_table;
```

## total_charge_cache 
UDF。查询cache容量的消耗情况。

需要在segment上执行。

可通过如下sql语句得到其在每个segment上执行结果
```sql
SELECT gp_segment_id, total_charge_cache()
FROM gp_dist_random('gp_id')
ORDER BY gp_segment_id;
```

## reset_cache
UDF。根据指定容量，重建cache。

需要在segment上执行。

| 参数 | 含义|
| --- | --- |
| capacity BIGINT | 期望的cache的容量 |

## prune_cache
UDF。清理cache中没有在使用中的缓存条目。

需要在segment上执行

# 编译安装
1. 本插件依赖于greenplum，需在其环境下编译

2. 本插件依赖于faiss，其git仓库使用faiss作为子模块，编译前需要手动初始化更新faiss子模块（```git submodule update --init --recursive```），并安装相关依赖，如boost、openblas。可参考[faiss/INSTALL.md](https://github.com/facebookresearch/faiss/blob/main/INSTALL.md)

3. ```make``` 会编译faiss，并将相关动态库install至greenplum期望的动态库加载路径下。

4. ```make install``` 会编译cache静态库和插件，并install。

5. 需要确保插件文件和依赖的动态库同步到所有节点所对应的目录下。

6. ```psql -c "create extension vector_recall;"```

# 其他

**cache**文件夹下是cache相关代码。

**sql/vector_recall.sql**和**expected/vector_recall.out**是单元测试文件，可作为用例参考。

# License
gpdb-faiss-vector is developed by Alibaba and licensed under the MIT License
This product contains various third-party components under other open source licenses.
See the NOTICE file for more information.

# 联系我们
阿里妈妈广告技术部数据引擎团队，负责开发面向百万广告主的数据产品和计算引擎，支持万亿级数据毫秒级交互式人群圈选、定向和洞察分析，以及在秒级进行百亿级数据的即席的广告效果分析，具有目前业界最复杂，规模最大的人群定向能力，支撑阿里巴巴最核心的广告营收业务。

gpdb-faiss-vector钉钉交流群：**44535363**
![dingtalk](dingtalk.jpg)
