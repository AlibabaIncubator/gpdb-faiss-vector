# vector_recall
vector_recall extension for greenplum-db

[faiss](https://github.com/facebookresearch/faiss)是用于高效相似性搜索和密集向量聚类的库。

[greenplum](https://github.com/greenplum-db/gpdb)是基于PostgreSQL用于分析的大规模并行处理架构的分布式数据库

本插件是将faiss集成到greenplum数据库中，以提供向量召回的能力。特点如下：
* 使用数据库的bytea数据类型存放faiss index字节序列。
* 提供了创建，训练和搜索faiss index等sql函数。
* 创建函数使用工厂模式，可以完美支持faiss各种index。
* 搜索函数支持topk搜索和阈值搜索。
* 内置cache，可以缓存faiss index内存对象，节省数据库读取faiss index字节序列和将其反序列化为内存对象的耗时（实践发现，这两步是主要耗时环节）。

# 数据类型

## __vector_index_search_results 
搜索函数返回的数据类型

| 参数 | 含义| 
| --- | --- |
|query_vector |查询向量值（数据数组）|
|query_idx|查询向量的ID|
|vector_idxs |查询向量所有最近邻的ID数组（如求TOP1000，则vector_idxs为1000个bigint的数组，按距离从小到大。如搜索结果不足1000，则返回数组大小会小于1000）|
|distances |与vector_idxs一一对应的距离（数组大小与vector_idxs相同）|

## __topk_merge_result
根据多个局部topk结果计算得到的全局topk数据类型

| 参数 | 含义| 
| --- | --- |
|idxs |聚合所有局部topk结果后得到的最终topk结果，长度取决于输入时的topk参数|
|distances |聚合后得到的距离结果，从小到大排序，与idxs一一对应|

# 函数

## array_1d_extend
将多个1维real数组extend成一个大的real数组。faiss c api规定要输入一维float数组，即使是实际要输入多个向量。

## faiss_index_create
对应于faiss的*faiss_index_factory*，创建faiss index。

采用工厂模式，index的类型（*index_desc*）用字符串描述，可参考[The index factory](https://github.com/facebookresearch/faiss/wiki/The-index-factory)。另外*metric_type*可参考[MetricType.h](https://github.com/facebookresearch/faiss/blob/main/faiss/MetricType.h)


## faiss_index_train
对应于faiss的*faiss_Index_train*，根据输入的向量训练faiss index。

## faiss_index_add
对应于faiss的*faiss_Index_add*，将向量添加到faiss index里。

*vector_idxs BIGINT[]* 参数可选，默认NULL，被查询向量对应的ID数组（长度为查询向量个数）。如果不提供该输入，则在执行vector_index_search时返回的vector_idx为从0开始按构建索引时插入顺序自动累计的默认ID。

## faiss_index_set_runtime_parameters
对应于faiss的*faiss_ParameterSpace_set_index_parameters* ，设置faiss index运行时参数。

其底层借助[The ParameterSpace object](https://github.com/facebookresearch/faiss/wiki/Index-IO,-cloning-and-hyper-parameter-tuning#the-parameterspace-object)进行运行时参数的设置。也是工厂模式

## faiss_index_reset
对应于faiss的*faiss_Index_reset*

## create_index_agg
辅助函数，封装了create、train、add和set runtime parameters

## faiss_index_search
对应于faiss的*faiss_Index_search*，从faiss index中搜索并返回topk的向量们。

*query_idxs BIGINT[]* 参数可选，默认为NULL，被查询向量对应的ID数组（长度为查询向量个数）。如果提供该输入，则在输出结果中返回的查询向量对应的ID

*preserve_vector bool* 参数可选，默认NULL，表示输出结果中是否保留查询向量的值

## faiss_index_range_search
对应于faiss的*faiss_Index_range_search*，从faiss index中搜索并返回距离小于阈值的向量们。

## topk_merge
用于合并多个局部topk为一个全局topk。可用于处理faiss_index_search的输出。

*idxs BIGINT[]* 表示向量的id号

*distance REAL[]* 表示向量的距离，应是有序数组

*topk INT* 表示期望的全局的topk的k

## total_charge_cache 
查询cache使用情况。

需要在segment上执行。
可通过如下sql语句得到其在每个segment上执行结果
```sql
SELECT gp_segment_id, total_charge_cache()
FROM gp_dist_random('gp_id')
ORDER BY gp_segment_id;
```
## reset_cache
根据指定容量，重建cache。

需要在segment上执行。
## prune_cache
清理cache中无用缓存项。

需要在segment上执行

# 编译安装
本插件依赖于greenplum，需在其环境下编译

本插件依赖于faiss，其git仓库使用faiss作为子模块，编译前需要手动初始化更新faiss子模块（```git submodule update --init --recursive```），并安装相关依赖，如boost、openblas。可参考[faiss/INSTALL.md](https://github.com/facebookresearch/faiss/blob/main/INSTALL.md)

```make``` 会编译faiss，并将相关动态库install至greenplum期望的动态库加载路径下。

```make install``` 会编译cache静态库和插件，并install。

需要确保插件文件和依赖的动态库同步到所有节点所对应的目录下。

```psql -c "create extension vector_recall;"```

# 其他

**cache**文件夹下是cache相关代码，移植自[leveldb](https://github.com/google/leveldb)

**sql/vector_recall.sql**和**expected/vector_recall.out**是单元测试文件，可作为用例参考。

# License
gpdb-faiss-vector is developed by Alibaba and licensed under the MIT License
This product contains various third-party components under other open source licenses.
See the NOTICE file for more information.

