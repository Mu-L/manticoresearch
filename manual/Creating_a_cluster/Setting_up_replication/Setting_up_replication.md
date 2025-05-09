# Setting up replication

With Manticore, write transactions (such as `INSERT`, `REPLACE`, `DELETE`, `TRUNCATE`, `UPDATE`, `COMMIT`) can be replicated to other cluster nodes before the transaction is fully applied on the current node. Currently, replication is supported for `percolate`, `rt` and `distributed` tables in Linux and macOS.

[Native Windows binaries](../../Installation/Windows.md#Installing-Manticore-as-native-Windows-binaries) for Manticore do not support replication. We recommend [installing Manticore via WSL](../../Installation/Windows.md#Installing-or-enabling-WSL2) (Windows Subsystem for Linux).

On [macOS](../../Installation/MacOS.md), replication has limited support and is recommended only for development purposes.

Manticore's replication is powered by the [Galera library](https://github.com/codership/galera) and boasts several impressive features:

* True multi-master: read and write to any node at any time.
* [Virtually synchronous replication](https://galeracluster.com/library/documentation/overview.html) no slave lag and no data loss after a node crash.
* Hot standby: no downtime during failover (since there is no failover).
* Tightly coupled: all nodes hold the same state and no diverged data between nodes is allowed.
* Automatic node provisioning: no need to manually backup the database and restore it on a new node.
* Easy to use and deploy.
* Detection and automatic eviction of unreliable nodes.
* Certification-based replication.

To set up replication in Manticore Search:

* The [data_dir](../../Server_settings/Searchd.md#data_dir) option must be set in the "searchd" section of the configuration file. Replication is not supported in plain mode.
* A [listen](../../Server_settings/Searchd.md#listen)  directive must be specified, containing an IP address accessible by other nodes, or a [node_address](../../Server_settings/Searchd.md#node_address) with an accessible IP address.
* Optionally, you can set unique values for [server_id](../../Server_settings/Searchd.md#server_id) on each cluster node. If no value is set, the node will attempt to use the MAC address or a random number to generate the `server_id`.

If there is no `replication` [listen](../../Server_settings/Searchd.md#listen) directive set, Manticore will use the first two free ports in the range of 200 ports after the default protocol listening port for each created cluster. To set replication ports manually, the [listen](../../Server_settings/Searchd.md#listen) directive (of `replication` type) port range must be defined and the address/port range pairs must not intersect between different nodes on the same server. As a rule of thumb, the port range should specify at least two ports per cluster. When you define a replication listener with a port range (e.g., `listen = 192.168.0.1:9320-9328:replication`), Manticore doesn't immediately start listening on these ports. Instead, it will take random free ports from the specified range only when you start using replication.

## Replication cluster

A replication cluster is a group of nodes in which a write transaction is replicated. Replication is set up on a per-table basis, meaning that one table can only belong to one cluster. There is no limit on the number of tables that a cluster can have. All transactions such as `INSERT`, `REPLACE`, `DELETE`, `TRUNCATE` on any percolate or real-time table that belongs to a cluster are replicated to all the other nodes in that cluster. [Distributed](../../Creating_a_table/Creating_a_distributed_table/Creating_a_distributed_table.md#Creating-a-distributed-table) tables can also be part of the replication process. Replication is multi-master, so writes to any node or multiple nodes simultaneously will work just as well.

To create a cluster, you can typically use the command [create cluster](../../Creating_a_cluster/Setting_up_replication/Creating_a_replication_cluster.md#Creating-a-replication-cluster) with `CREATE CLUSTER <cluster name>`, and to join a cluster, you can use [join cluster](../../Creating_a_cluster/Setting_up_replication/Joining_a_replication_cluster.md#Joining-a-replication-cluster) with `JOIN CLUSTER <cluster name> at 'host:port'`. However, in some rare cases, you may want to fine-tune the behavior of `CREATE/JOIN CLUSTER`. The available options are:

### name

This option specifies the name of the cluster. It should be unique among all the clusters in the system.

> **Note:** The maximum allowable hostname length for the `JOIN` command is **253** characters. If you exceed this limit, searchd will generate an error.

### path

The path option specifies the data directory for [write-set cache replication](https://galeracluster.com/library/documentation/state-transfer.html#state-transfer-gcache) and incoming tables from other nodes. This value should be unique among all the clusters in the system and should be specified as a relative path to the [data_dir](../../Server_settings/Searchd.md#data_dir). directory. By default, it is set to the value of [data_dir](../../Server_settings/Searchd.md#data_dir).

### nodes

The `nodes` option is a list of address:port pairs for all the nodes in the cluster, separated by commas. This list should be obtained using the node's API interface and can include the address of the current node as well. It is used to join the node to the cluster and to rejoin it after a restart.

### options

The `options` option allows you to pass additional options directly to the Galera replication plugin, as described in the [Galera Documentation Parameters](https://galeracluster.com/library/documentation/galera-parameters.html)

## Write statements

<!-- example write statements 1 -->
When working with a replication cluster, all write statements such as  `INSERT`, `REPLACE`, `DELETE`, `TRUNCATE`, `UPDATE` that modify the content of a cluster's table must use the`cluster_name:table_name` expression instead of the table name. This ensures that the changes are propagated to all replicas in the cluster. If the correct expression is not used, an error will be triggered.

In the JSON interface, the `cluster` property must be set along with the `table` name for all write statements to a cluster's table. Failure to set the `cluster` property will result in an error.

The [Auto ID](../../Data_creation_and_modification/Adding_documents_to_a_table/Adding_documents_to_a_real-time_table.md#Auto-ID) for a table in a cluster should be valid as long as the [server_id](../../Server_settings/Searchd.md#server_id) is correctly configured.

<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
INSERT INTO posts:weekly_index VALUES ( 'iphone case' )
TRUNCATE RTINDEX click_query:weekly_index
UPDATE INTO posts:rt_tags SET tags=(101, 302, 304) WHERE MATCH ('use') AND id IN (1,101,201)
DELETE FROM clicks:rt WHERE MATCH ('dumy') AND gid>206
```

<!-- request JSON -->

```json
POST /insert -d '
{
  "cluster":"posts",
  "table":"weekly_index",
  "doc":
  {
    "title" : "iphone case",
    "price" : 19.85
  }
}'
POST /delete -d '
{
  "cluster":"posts",
  "table": "weekly_index",
  "id":1
}'
```

<!-- request PHP -->

```php
$index->addDocuments([
        1, ['title' => 'iphone case', 'price' => 19.85]
]);
$index->deleteDocument(1);
```

<!-- intro -->
##### Python:

<!-- request Python -->

``` python
indexApi.insert({"cluster":"posts","table":"weekly_index","doc":{"title":"iphone case","price":19.85}})
indexApi.delete({"cluster":"posts","table":"weekly_index","id":1})
```

<!-- intro -->
##### Python-asyncio:

<!-- request Python-asyncio -->

``` python
await indexApi.insert({"cluster":"posts","table":"weekly_index","doc":{"title":"iphone case","price":19.85}})
await indexApi.delete({"cluster":"posts","table":"weekly_index","id":1})
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

``` javascript
res = await indexApi.insert({"cluster":"posts","table":"weekly_index","doc":{"title":"iphone case","price":19.85}});
 res = await indexApi.delete({"cluster":"posts","table":"weekly_index","id":1});
```

<!-- intro -->
##### java:

<!-- request Java -->

``` java
InsertDocumentRequest newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","Crossbody Bag with Tassel");
    put("price",19.85);
}};
newdoc.index("weekly_index").cluster("posts").id(1L).setDoc(doc);
sqlresult = indexApi.insert(newdoc);

DeleteDocumentRequest deleteRequest = new DeleteDocumentRequest();
deleteRequest.index("weekly_index").cluster("posts").setId(1L);
indexApi.delete(deleteRequest);

```

<!-- intro -->
##### C#:

<!-- request C# -->

``` clike
Dictionary<string, Object> doc = new Dictionary<string, Object>();
doc.Add("title", "Crossbody Bag with Tassel");
doc.Add("price", 19.85);
InsertDocumentRequest newdoc = new InsertDocumentRequest(table: "weekly_index", cluster:posts, id: 1, doc: doc);
var sqlresult = indexApi.Insert(newdoc);

DeleteDocumentRequest deleteDocumentRequest = new DeleteDocumentRequest(table: "weekly_index", cluster: "posts", id: 1);
indexApi.Delete(deleteDocumentRequest);
```

<!-- intro -->
##### Rust:

<!-- request Rust -->

``` rust
let mut doc = HashMap::new();
doc.insert("title".to_string(), serde_json::json!("Crossbody Bag with Tassel"));
doc.insert("price".to_string(), serde_json::json!(19.85));
let insert_req = InsertDocumentRequest {
    table: serde_json::json!("weekly_index"),
    doc: serde_json::json!(doc),
    cluster: serde_json::json!("posts"),
    id: serde_json::json!(1),
};
let insert_res = index_api.insert(insert_req).await;

let delete_req = DeleteDocumentRequest {
    table: serde_json::json!("weekly_index"),
    cluster: serde_json::json!("posts"),
    id: serde_json::json!(1),
};
index_api.delete(delete_req).await;
```

<!-- end -->

## Read statements

<!-- example write statements 2 -->
Read statements such as `SELECT`, `CALL PQ`, `DESCRIBE` can either use regular table names that are not prepended with a cluster name, or they can use the  `cluster_name:table_name`format. If the latter is used, the `cluster_name` component is ignored.

When using the HTTP endpoint `json/search`, the `cluster` property can be specified if desired, but it can also be omitted.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
SELECT * FROM weekly_index
CALL PQ('posts:weekly_index', 'document is here')
```

<!-- request JSON -->

```json
POST /search -d '
{
  "cluster":"posts",
  "table":"weekly_index",
  "query":{"match":{"title":"keyword"}}
}'
POST /search -d '
{
  "table":"weekly_index",
  "query":{"match":{"title":"keyword"}}
}'
```

<!-- end -->

## Cluster parameters

<!-- example cluster parameters 1 -->
Replication plugin options can be adjusted using the `SET` statement.

A list of available options can be found in the [Galera Documentation Parameters](https://galeracluster.com/library/documentation/galera-parameters.html) .


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
SET CLUSTER click_query GLOBAL 'pc.bootstrap' = 1
```
<!-- request JSON -->

```json
POST /cli -d "
SET CLUSTER click_query GLOBAL 'pc.bootstrap' = 1
"
```
<!-- end -->

## Cluster with diverged nodes

<!-- example cluster with diverged nodes  1 -->
It's possible for replicated nodes to diverge from one another, leading to a state where all nodes are labeled as `non-primary`. This can occur as a result of a network split between nodes, a cluster crash, or if the replication plugin experiences an exception when determining the `primary component`. In such a scenario, it's necessary to select a node and promote it to the role of `primary component`.

To identify the node that needs to be promoted, you should compare the `last_committed` cluster status variable value on all nodes. If all the servers are currently running, there's no need to restart the cluster. Instead, you can simply promote the node with the highest last_committed value to the `primary component` using the `SET` statement (as demonstrated in the example).

The other nodes will then reconnect to the primary component and resynchronize their data based on this node.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
SET CLUSTER posts GLOBAL 'pc.bootstrap' = 1
```
<!-- request JSON -->

```json
POST /cli -d "
SET CLUSTER posts GLOBAL 'pc.bootstrap' = 1
"
```
<!-- end -->

## Replication and cluster

<!-- example replication and cluster 1 -->
To use replication, you need to define one [listen](../../Server_settings/Searchd.md#listen) port for SphinxAPI protocol and one  [listen](../../Server_settings/Searchd.md#listen) for replication address and port range in the configuration file. Also, specify the  [data_dir](../../Server_settings/Searchd.md#data_dir) folder to receive incoming tables.


<!-- intro -->
##### ini:

<!-- request ini -->
```ini
searchd {
  listen   = 9312
  listen   = 192.168.1.101:9360-9370:replication
  data_dir = /var/lib/manticore/
  ...
 }
```
<!-- end -->

<!-- example replication and cluster 2 -->
To replicate tables, you must create a cluster on the server that has the local tables to be replicated.

<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
CREATE CLUSTER posts
```

<!-- request JSON -->

```json
POST /cli -d "
CREATE CLUSTER posts
"
```

<!-- request PHP -->

```php
$params = [
    'cluster' => 'posts'
    ]
];
$response = $client->cluster()->create($params);
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('CREATE CLUSTER posts')
```

<!-- intro -->
##### Python-asyncio:

<!-- request Python-asyncio -->

```python
await utilsApi.sql('CREATE CLUSTER posts')
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

```javascript
res = await utilsApi.sql('CREATE CLUSTER posts');
```

<!-- intro -->
##### Java:

<!-- request Java -->

```java
utilsApi.sql("CREATE CLUSTER posts");

```

<!-- intro -->
##### C#:

<!-- request C# -->

```clike
utilsApi.Sql("CREATE CLUSTER posts");
```

<!-- intro -->
##### Rust:

<!-- request Rust -->

```rust
utils_api.sql("CREATE CLUSTER posts", Some(true)).await;
```

<!-- end -->

<!-- example replication and cluster 3 -->
Add these local tables to the cluster


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
ALTER CLUSTER posts ADD pq_title
ALTER CLUSTER posts ADD pq_clicks
```

<!-- request JSON -->

```json
POST /cli -d "
ALTER CLUSTER posts ADD pq_title
"
POST /cli -d "
ALTER CLUSTER posts ADD pq_clicks
"
```

<!-- request PHP -->

```php
$params = [
  'cluster' => 'posts',
  'body' => [
     'operation' => 'add',
     'table' => 'pq_title'

  ]
];
$response = $client->cluster()->alter($params);
$params = [
  'cluster' => 'posts',
  'body' => [
     'operation' => 'add',
     'table' => 'pq_clicks'

  ]
];
$response = $client->cluster()->alter($params);   
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('ALTER CLUSTER posts ADD pq_title')
utilsApi.sql('ALTER CLUSTER posts ADD pq_clicks')
```

<!-- intro -->
##### Python-asyncio:

<!-- request Python-asyncio -->

```python
await utilsApi.sql('ALTER CLUSTER posts ADD pq_title')
await utilsApi.sql('ALTER CLUSTER posts ADD pq_clicks')
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

```javascript
res = await utilsApi.sql('ALTER CLUSTER posts ADD pq_title');
res = await utilsApi.sql('ALTER CLUSTER posts ADD pq_clicks');
```

<!-- intro -->
##### Java:

<!-- request Java -->

```java
utilsApi.sql("ALTER CLUSTER posts ADD pq_title");
utilsApi.sql("ALTER CLUSTER posts ADD pq_clicks");
```

<!-- intro -->
##### C#:

<!-- request C# -->

```clike
utilsApi.Sql("ALTER CLUSTER posts ADD pq_title");
utilsApi.Sql("ALTER CLUSTER posts ADD pq_clicks");
```

<!-- intro -->
##### Rust:

<!-- request Rust -->

```rust
utils_api.sql("ALTER CLUSTER posts ADD pq_title", Some(true)).await;
utils_api.sql("ALTER CLUSTER posts ADD pq_clicks", Some(true)).await;
```

<!-- end -->

<!-- example replication and cluster 4 -->
All other nodes that wish to receive a replica of the cluster's tables should join the cluster as follows:


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
JOIN CLUSTER posts AT '192.168.1.101:9312'
```

<!-- request JSON -->

```json
POST /cli -d "
JOIN CLUSTER posts AT '192.168.1.101:9312'
"
```

<!-- request PHP -->

```php
$params = [
  'cluster' => 'posts',
  'body' => [
      '192.168.1.101:9312'
  ]
];
$response = $client->cluster->join($params);
```
<!-- intro -->
##### Python:

<!-- request Python -->

```python
utilsApi.sql('JOIN CLUSTER posts AT \'192.168.1.101:9312\'')
```

<!-- intro -->
##### Python-asyncio:

<!-- request Python-asyncio -->

```python
await utilsApi.sql('JOIN CLUSTER posts AT \'192.168.1.101:9312\'')
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

```javascript
res = await utilsApi.sql('JOIN CLUSTER posts AT \'192.168.1.101:9312\'');
```

<!-- intro -->
##### Java:

<!-- request Java -->

```java
utilsApi.sql("JOIN CLUSTER posts AT '192.168.1.101:9312'");

```

<!-- intro -->
##### C#:

<!-- request C# -->

```clike
utilsApi.Sql("JOIN CLUSTER posts AT '192.168.1.101:9312'");

```

<!-- intro -->
##### Rust:

<!-- request Rust -->

```rust
utils_api.sql("JOIN CLUSTER posts AT '192.168.1.101:9312'", Some(true)).await;

```
<!-- end -->

<!-- example replication and cluster 5 -->
When running queries, prepend the table name with the cluster name `posts`: or use the `cluster` property for HTTP request object.


<!-- intro -->
##### SQL:

<!-- request SQL -->

```sql
INSERT INTO posts:pq_title VALUES ( 3, 'test me' )
```

<!-- request JSON -->

```json
POST /insert -d '
{
  "cluster":"posts",
  "table":"pq_title",
  "id": 3
  "doc":
  {
    "title" : "test me"
  }
}'
```

<!-- request PHP -->

```php
$index->addDocuments([
        3, ['title' => 'test me']
]);

```
<!-- intro -->
##### Python:

<!-- request Python -->

``` python
indexApi.insert({"cluster":"posts","table":"pq_title","id":3"doc":{"title":"test me"}})

```

<!-- intro -->
##### Python-asyncio:

<!-- request Python-asyncio -->

``` python
await indexApi.insert({"cluster":"posts","table":"pq_title","id":3"doc":{"title":"test me"}})
```

<!-- intro -->
##### Javascript:

<!-- request Javascript -->

``` javascript
res = await indexApi.insert({"cluster":"posts","table":"pq_title","id":3"doc":{"title":"test me"}});
```

<!-- intro -->
##### java:

<!-- request Java -->

``` java
InsertDocumentRequest newdoc = new InsertDocumentRequest();
HashMap<String,Object> doc = new HashMap<String,Object>(){{
    put("title","test me");
}};
newdoc.index("pq_title").cluster("posts").id(3L).setDoc(doc);
sqlresult = indexApi.insert(newdoc);
```

<!-- intro -->
##### C#:

<!-- request C# -->

``` clike
Dictionary<string, Object> doc = new Dictionary<string, Object>();
doc.Add("title", "test me");
InsertDocumentRequest newdoc = new InsertDocumentRequest(index: "pq_title", cluster: "posts", id: 3, doc: doc);
var sqlresult = indexApi.Insert(newdoc);
```

<!-- intro -->
##### Rust:

<!-- request Rust -->

``` rust
let mut doc = HashMap::new();
doc.insert("title".to_string(), serde_json::json!("test me"));
let insert_req = InsertDocumentRequest {
    table: serde_json::json!("pq_title"),
    doc: serde_json::json!(doc),
    cluster: serde_json::json!("posts"),
    id: serde_json::json!(3),
};
let insert_res = index_api.insert(insert_req).await;
```
<!-- end -->

All queries that modify tables in the cluster are now replicated to all nodes in the cluster.
<!-- proofread -->
