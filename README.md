# vector_recall
vector_recall extension for greenplum-db

# 类型

## __vector_index_search_results 
| 参数 | 含义| 
| --- | --- |
|query_vector |查询向量值（数据数组）|
|query_idx|查询向量的ID|
|vector_idx | 查询向量所有最近邻的ID数组（如求TOP1000，则vector_idx为1000个bigint的数组，按距离从小到大。如搜索结果不足1000，则返回数组大小会小于1000）|
|distance |与vector_idx一一对应的距离（数组大小与vector_idx相同）|

## __topk_merge_result
| 参数 | 含义| 
| --- | --- |
|idxs |聚合所有局部topk结果后得到的最终topk结果，长度取决于输入时的topk参数|
|distance |聚合后得到的距离结果，从小到大排序，与idxs一一对应|

# 函数

## array_1d_extend
将多个1维real数组extend成一个大的real数组

## faiss_index_create
对应于faiss的*faiss_index_factory*，创建faiss index。
工厂模式，index的类型用字符串描述

[The index factory](https://github.com/facebookresearch/faiss/wiki/The-index-factory)

[full list of metrics](https://github.com/facebookresearch/faiss/blob/main/faiss/MetricType.h#L44)


## faiss_index_train
对应于faiss的*faiss_Index_train*，根据输入的向量训练faiss index。

## faiss_index_add
对应于faiss的*faiss_Index_add*，将向量添加到faiss index里。

*vector_idxs BIGINT[]* 参数可选，默认NULL，被查询向量对应的ID数组（长度为查询向量个数）。如果不提供该输入，则在执行vector_index_search时返回的vector_idx为从0开始按构建索引时插入顺序自动累计的默认ID。

## faiss_index_set_runtime_parameters
对应于faiss的*faiss_ParameterSpace_set_index_parameters* ，设置faiss index运行时参数。
借助[The ParameterSpace object](https://github.com/facebookresearch/faiss/wiki/Index-IO,-cloning-and-hyper-parameter-tuning#the-parameterspace-object)进行运行时参数的设置。也是工厂模式

## faiss_index_reset
对应于faiss的*faiss_Index_reset*

## create_index_agg
辅助函数，封装了create、train、add和set runtime parameters

## faiss_index_search
对应于faiss的*faiss_Index_search*

*query_idxs BIGINT[]* 参数可选，默认为NULL，被查询向量对应的ID数组（长度为查询向量个数）。如果提供该输入，则在输出结果中返回的查询向量对应的ID

*preserve_vector bool* 参数可选，默认NULL，表示输出结果中是否保留查询向量的值

## faiss_index_range_search
对应于faiss的*faiss_Index_range_search*

## topk_merge
用于合并多个局部topk为一个全局topk。可用于处理faiss_index_search的输出。

*idxs BIGINT[]* 表示向量的id号

*distance REAL[]* 表示向量的距离，应是有序数组

*topk INT* 表示期望的全局的topk的k

## reset_cache total_charge_cache prune_cache
对cache控制

使其在每个segment上执行
```sql
SELECT gp_segment_id,
    total_charge_cache()
FROM gp_dist_random('gp_id')
ORDER BY gp_segment_id;
```

# 相关链接

[向量批量召回计划](https://yuque.antfin-inc.com/docs/share/684c1993-8d7b-46d4-a9a8-f77552836db4)

[向量批量召回相关UDF说明](https://yuque.antfin.com/docs/share/162d52be-c9dd-4bbd-9e92-4ef128195ab3?#)

# License
gpdb-faiss-vector is developed by Alibaba and licensed under the MIT License
This product contains various third-party components under other open source licenses.
See the NOTICE file for more information.

