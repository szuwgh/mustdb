# MustDB — Claude Code 项目指令

## 设计哲学
1. 一切接插件
2. 小内核 大扩展
3. **嵌入式轻量 PG 风格实现**：MustDB 对齐 PostgreSQL 中确实需要的
   存储、表访问、缓冲、WAL、事务、MVCC 和 checkpoint 的接口语义、命名和代码组织，
   但目标不是 PostgreSQL 的 1:1 完整复制。不得为了形式对齐引入 backend、SQL 执行器、
   进程级全局状态、分布式组件或其他嵌入式场景不需要的复杂机制；每个与 PG 的差异都应
   保持边界清晰，并以嵌入式生命周期、资源占用和可测试性为取舍依据。

## 第一黄金目标：可复用检索库优先
1. MustDB 的向量能力首先要沉淀为**可复用向量算法库**：`tmp/src/vector_algo` 是第一目标，
   负责 DiskANN/Vamana、IVF、SBQ/PQ/int8/int4/RabitQ、distance/top-k、sparse、
   multivector 等纯算法能力。
2. MustDB 的全文能力首先要沉淀为**可复用全文检索库**：`tmp/src/fulltext_algo` 是第一目标，
   负责 tokenizer/analyzer、term dictionary、posting/doclist/poslist、query AST、
   BM25/top-k、merge、bitpacked posting/WAND 等纯检索能力。
3. `tmp/src/vector_index` 和 `tmp/src/fulltext_index` 的地位是 MustDB 封装层：
   只负责 segment、manifest、mutable/immutable 生命周期、StorageManager fork/extent、
   tombstone/delete、compact、cache、recovery、telemetry，以及把 heap CTID/MVCC
   映射到可复用算法库。
4. `vector_algo` / `fulltext_algo` 不得反向依赖 `vector_index` / `fulltext_index`，
   不得包含 StorageManager、EmbeddingStore、RowStore、manifest、fork/page range、
   CTID/ItemPtr 生命周期等 MustDB 存储细节。需要外部数据时，用中立 callback、
   byte slice、segment-local id 或纯算法类型表达。
5. 新功能优先判断：能否先作为 `*_algo` 的独立单元被测试和复用；只有算法库边界稳定后，
   才进入 `*_index` 做 MustDB 单文件 segment 封装。

## 第二黄金目标：Agent Memory 双引擎

Agent Memory Database 由 Rust Memory API 编排两个独立引擎：MustDB 负责“找什么”，
`libostore` 负责“内容在哪里、怎样可靠保存”。两者不得互相吞并职责。

```
Rust Memory API
├── MustDB
│   ├── memory metadata / facts / chunk / relation
│   ├── summary、source、time、permission、object_id/content_hash 引用
│   ├── fulltext_index（关键词 / BM25）
│   └── EmbeddingStore + vector_index（语义召回）
└── libostore
    ├── conversation JSONL / Markdown
    ├── PDF、image、audio、code、tool output、generated artifact
    └── checkpoint / snapshot 等大对象
```

1. `libostore` 是独立、可复用的纯 C object storage library；它不依赖 MustDB、
   `StorageManager`、RowStore、EmbeddingStore、vector/fulltext index 或 MustDB 文件格式。
   它未来可被备份、artifact、模型缓存和其他数据库 wrapper 复用。`libostore` core
   不得直接 include MustDB 头文件；只有独立的 MustDB adapter/plugin 可以依赖 MustDB。
2. MustDB 是检索、关联与引用元数据层，不承担大型原文/附件的主存储；检索命中返回
   `object_id` 与 byte/chunk range，由 Rust Memory API 调 `libostore` 读取内容。
   小型摘要或必要文本可使用 heap/简化 TOAST，但不能把它当作大对象主路径。
3. 对象优先采用 immutable content-addressed 语义：`object_id = BLAKE3(payload)`。
   `libostore` 负责 segment/blob、chunk、checksum、staging、atomic publish、crash
   recovery、scrub、backup 与 GC；它不执行 vector/fulltext search。
4. `libostore` 的元数据必须分为两层，避免把 core 演化成第二个通用数据库：

   ```text
   物理对象元数据 object.meta（始终由 libostore 保存）
     = format / object_id / state / logical_size / chunk layout /
       per-chunk checksum / compression / encryption / EC placement

   逻辑对象目录 OStoreMetaEngine（可插拔）
     = object_id -> location / state / generation / delete marker /
       listing / GC epoch / optional user attributes
   ```

   第一阶段的默认 `FileMetaEngine` 使用按文件的对象目录，布局类似 MinIO：
   `objects/ab/cd/<blake3>/object.meta`，配合 `staging/` 和 `trash/`。它只做对象
   catalog，不实现 SQL、全文、向量或业务关系。`object.meta` 让 catalog 损坏时仍可扫描
   objects 重建，也可以验证 catalog 是否指向存在且完整的 blob。
5. `OStoreMetaEngine` 是稳定的窄插件接口，至少覆盖 lookup、prepare/commit publish、
   mark delete、iterate 与 checkpoint。默认后端是 `FileMetaEngine`；可选
   `MustDBMetaEngine` adapter 用 MustDB 表、事务、WAL 与索引持久化逻辑对象目录。
   Memory Database 启用该 adapter 后，object catalog、memory/chunk、全文/向量索引引用
   可以进入同一 MustDB 事务，但物理 `object.meta` 仍由 `libostore` 保留。
6. 跨引擎引用一致性采用“先 object、后 metadata”顺序：先让 `libostore` durable publish
   object，再在 MustDB 事务中写入 memory/chunk/index/object_id 引用，最后提交事务。
   失败留下的无引用 object 由延迟 GC/reconcile 清理，绝不能提交指向不存在 object 的引用。
   删除先删除 MustDB 引用，确认无引用且 reader 不再 pin 后才可回收 object。
7. `libostore` 第一阶段只面向单机可靠性：checksum、staging/recovery、scrub、可验证
   snapshot/backup 与单盘故障诊断。多盘先实现副本/镜像、placement、degraded read、
   rebuild；Reed-Solomon EC 仅在 shard 分布于独立物理盘时启用。单盘 EC 没有恢复价值，
   单机多盘 EC 仍不能替代离机备份。

## 规则
1. 每次回复我前请叫我师父
2. 这个项目要和/home/unvdb/cproject/vecobs协作
3. `docs/` 目录不能提交到 Git；即使需要写设计文档或计划文档，也只能保留为本地 ignored 文件，不得 `git add -f docs/...`
4. AI 不得私自创建 Git 提交记录；只有用户在当前对话中明确要求提交时，才允许执行 `git commit`

## 数据库
安装目录为/home/unvdb/unvdb-tx/bin ，数据目录是/home/unvdb/unvdb-data，客户端为：ud_sql, 端口：5678

## 项目结构

```
mustdb/
├── src/                  # 产品化核心基础库 (libmustdb.a) — AI 不得修改，构建：make -C src
│   ├── include/          # catalog / storage / heap / table / segment / wal 等头文件
│   ├── catalog/          # Catalog / SchemaCatalogEntry
│   ├── storage/          # buffer(clock/pool) / index / table(heap/segment/storage/store/table)
│   ├── transaction/      # 事务
│   ├── wal/              # WAL
│   └── ...               # parse / utils
│
├── tests/                # src 层测试（主要通过 tmp/tests 移植）
│
├── pg_mustdb/            # PG 插件产品化核心 — AI 不得修改
│
└── tmp/                  # 活跃开发实验场（AI 可修改）
    ├── src/              # 嵌入式存储引擎实验场
    │   ├── vector_algo/  # 可复用向量算法库：DiskANN / IVF-Flat / SBQ / PQ / top-k / 距离算法
    │   ├── vector_index/ # MustDB 向量 segment 封装：ViIndex LSM / manifest / reader / cache / io / storage
    │   ├── fulltext_algo/ # 可复用全文检索库：tokenizer / postings / query / BM25 / merge / compression
    │   ├── fulltext_index/ # MustDB 全文 segment 封装：manifest / mutable-immutable / storage / compact
    │   └── ...           # row_store / embedding_store / table_am / btree / pager 等
    ├── tests/            # tmp/src 测试（test_*.c，自动发现）
    └── pg_mustdb_tmp/    # PG 插件实验场
```

## AI 修改边界（强制）
- `tmp/pg_mustdb_tmp` 是PG插件实验场：AI 可以在这里验证方案、写原型、跑测试、积累可迁移经验。
- `tmp/src` 是实验场：AI 可以在这里验证方案、写原型、跑测试、积累可迁移经验。
- `src` 是产品化核心：AI **不得直接修改 `src/` 下任何代码或头文件**。
- `pg_mustdb` 是PG插件产品化核心：AI **不得直接修改 `pg_mustdb/` 下任何代码或头文件**。
- 如果用户要把 `tmp/src` 经验迁移到 `src`，AI 只能做分析、设计、拆任务、指出参考代码和风险；具体 `src` 代码由用户自己手写。
- AI 不得把 `tmp/src` 文件整块复制到 `src`，也不得用“清理/同步/顺手修复”为理由改动 `src`。
- 只有当用户在当前对话中明确写出“允许你修改 `src/...` 具体文件”时，AI 才能触碰对应文件；授权必须是文件级或任务级的，不能默认扩展到整个 `src/`。

## 构建与测试

```bash
# 构建并运行所有测试（主要命令）
make -C tmp test

# 仅构建
make -C tmp

# 重新构建基础库（修改 src/*.h 后必须执行）
make -C src clean && make -C src

# 清理测试产物
make -C tmp clean
```

**注意**：`src/Makefile` 无头文件依赖追踪，修改 `src/*.h` 后必须手动执行
`make -C src clean && make -C src`。

## 核心架构

### 分层总览

```
Collection / MustDbAccess                                  ← 公开 facade 与插件管理器
    ↓
MustDbRelation + MustDbIndexAm (BTree / Vector)            ← relation 与二级索引编排
    ↓
StorageTable / TableAm (TamHeapTable + TamColTable)        ← 表 AM vtable
    ↓
RowStore(heap)   EmbeddingStore(f32)   ColumnStore    ViIndex LSM(ANN)
    ↓
MustDbPager / BufferPool / page WAL                        ← 页面缓存、刷脏与页镜像
    ↓
MustDbStorageManager(fork 分配) / MustDbStorageDevice      ← 单文件存储设备
    ↓
TransactionManager (CLOG + WAL + Snapshot)                 ← 横切 MVCC / 恢复
```

`MustDbDatabase` 是唯一通用 runtime owner，拥有 SMGR、数据库级 BufferPool、
TransactionManager/WAL、Catalog、checkpoint participant 和 redo registry。
`MustDbRelation` 是打开的表句柄；`Collection` 是唯一公开的 document/vector/fulltext
便利 facade。`DocumentAccess`/`GraphAccess` 仅为内部插件实现，不打开数据库、不创建物理表。

数据库路径始终是精确 `.mustdb` 文件，WAL 始终由该路径派生。创建与打开严格分离：
`mustdb_database_create()` 只创建，`mustdb_database_open()` 只打开；不得根据路径存在性
在数据库层自动切换，也不得保留目录存储、调用方 WAL 路径或向量目录第二主路径。

### 单文件存储原则（嵌入式默认路线）

MustDB 的嵌入式存储目标是类似 SQLite / DuckDB 的**单文件数据库**：
一个 `.mustdb` 文件拷贝到其他机器或目录后，应该可以独立打开和使用。

- **主数据文件**：payload/RowStore、B-Tree、VectorIndex LSM segment、全文索引 segment、manifest/free map 等长期状态都应进入同一个 `.mustdb` 文件。
- **WAL 文件**：采用 sidecar WAL（例如 `.mustdb.wal`），用于事务提交与崩溃恢复；checkpoint 成功后按逻辑 `base_lsn` 回收前缀，不能重置或复用 page LSN。不要把长期 WAL 当成主数据长期组成部分。
- **分配单位**：单文件内部统一按 8KB page 分配；大对象、vector segment、FTS segment 使用连续或多段 page range / extent 存储。
- **Segment 首页头**：多 page segment 的第一页必须写 `MustDbSegmentHeader`，记录 `kind/segment_id/payload_offset/payload_size/page_count`。manifest/free map 仍是权威，header 只做自描述、校验和恢复辅助。
- **SMGR 边界**：嵌入式主路径只使用 `MustDbStorageManager` + `MustDbStorageDevice` vtable；上层模块不依赖 BlockManager，不直接假设目录文件，也不管理裸文件 offset。
- **fork 分配**：`MustDbStorageManager` 按 fork 组织存储，当前 fork 有 `META` / `HEAP` / `FSM` / `VM` / `OVERFLOW` / `EMBEDDING` / `VECTOR_SEGMENT` / `VECTOR_MANIFEST` / `FULLTEXT_SEGMENT` / `GRAPH_SEGMENT` / `FULLTEXT_MANIFEST`。relation 使用 fork-local block；SMGR 独占 `(storage_id,fork,local_block) -> global extent` 映射。大 segment extent 以 `MustDbStoragePageRange` 分配/回收。
- **存储设备**：`MustDbStorageDevice` vtable（read_at/write_at/mmap_range/sync/truncate/caps）把本地文件系统能力抽象成可替换后端；`MustDbPageIO` 是 fork 级的窄 read/write page adapter。
- **VectorIndex compact**：compact 时写新 segment page range → 发布新 manifest → 旧 segment page range 进入 free extent map。不得让旧 segment 永久悬挂导致文件无限增长。
- **free extent map**：长期写入和 compact 产生的空闲 page 必须回收到 free map；文件尾部连续空闲 page 可以 tail truncate。
- **VACUUM FULL**：走 PG 风格路线：扫描 live heap tuple → 重写新 `.mustdb` → 重建所有索引 → rename 替换旧文件。不要试图原地维护旧 CTID；VACUUM FULL 后旧 CTID/ItemPointer 语义上失效。
- **普通 VACUUM**：只做页内 prune、free_list/free map 回收和可复用空间维护，不移动 live tuple，不改变 CTID。
- **GPU 预留**：vector segment 在单文件中仍应保持大块连续布局，便于未来 mmap / pinned memory / GPU buffer 加载；不要把向量长期拆散进 heap tuple body。
- **PG 插件适配**：PG adapter 可以复用同一套核心文件格式/索引库，但 PG 侧只做 wrapper、AM、worker、错误处理和路径适配，不应把核心逻辑重新实现一份。

### 物理地址：ctid（唯一标识符）

```c
typedef ItemPtr NodePtr;   // {ip_blkid_hi, ip_blkid_lo, ip_posid} = 6 bytes
// 完全对齐 PostgreSQL ItemPointerData
```

- **没有 `next_internal_id` 或顺序 iid 计数器** — 已彻底消除
- `itemptr_pack(p) → u64`：用于 hmap / free_list / 磁盘格式存储
- `itemptr_unpack(u64) → ItemPtr`：反序列化
- `heap_ctid` ≠ `emb_ctid`：两者是不同地址空间的 ItemPtr（Option C 解耦设计）

### 存储引擎与 TableAm

`StorageTable` 现在只组合两个 TableAm 引擎；f32 向量不再作为 TableAm 引擎存在，
而是由独立的 `EmbeddingStore` 持有，向量指针通过 `ItemPtr emb_ctid` 与 heap 解耦。

```
StorageTable
├── TamHeapTable   — RowStore（MVCC 权威 + 全部用户 heap 列）
└── TamColTable    — ColumnStore/DataTable（标量列，append-only）

EmbeddingStore     — f32 向量 bytes（独立 store，不属于 StorageTable 引擎数组）
```

`TamHeapTable` 的每个 heap slot 格式为 `[TupleHdr (34B)][col_0][col_1]...`。
Tuple header 只保存 heap/MVCC 状态；稳定 `vector_id` 是普通内部列 `_vector_id`，
物理 embedding ref 只存在于 `EmbeddingStore` RefTable、ViIndex 和统一 vector WAL intent。

每个引擎实现 `TamRoutine` vtable（**数据库表的统一抽象**）：

```c
typedef struct {
    void (*append)(TableAm* am, const void* data);
    int  (*get)(TableAm* am, u64 seq_idx, void* out);
    u64  (*count)(TableAm* am);
    void (*destroy)(TableAm* am);
    void (*append_chunk)(TableAm* am, const MustDbChunk* chunk, TamInsertCtx* ctx);
    int  (*read_chunk)(TableAm* am, const TamReadCtx* ctx, MustDbChunk* out_chunk,
                       usize* out_idx, usize count);
    int  (*scan)(TableAm* am, TamScanCtx* ctx, MustDbChunk* out_chunk);
    int  (*get_hdr)(TableAm* am, ItemPtr ctid, TupleHdr* out);
    int  (*prepare_read)(TableAm* am, ItemPtr ctid, TamReadCtx* ctx);
    int  (*update)(TableAm* am, MustDbVector* v, const Datum* payloads, u64 null_bits,
                   TamUpdateCtx* ctx);
    int  (*delete)(TableAm* am, TamDeleteCtx* ctx);
    void (*free_read)(TableAm* am, TamReadCtx* ctx);
} TamRoutine;
```

`TableAm` 基结构用 `insert_phase`（PRE_HEAP/HEAP/POST_HEAP）、`read_order`、
`scan_order`、`out_ncols`、`payload_ncols` 声明各引擎的调度位置，orchestrator
不需要硬编码具体引擎类型。当前 `insert_phase` 只有 HEAP/POST_HEAP 两类实际参与。

### MustDbChunk / MustDbVector 设计哲学

**MustDbVector 是统一的向量原语**——既可以是嵌入式向量，也可以是行式向量：

```c
typedef struct {
    TypeID     type;   // TYPE_FLOAT32 / TYPE_INT64 / ...
    usize      count;  // count=dim → 嵌入向量；count=1 → 标量
    data_ptr_t data;   // 指向实际数据的指针
} MustDbVector;
```

**MustDbChunk 是统一的批量数据容器**，`append_chunk`（数据入）与 `read_chunk`（数据出）完全对称：

```
MUSTDBCHUNK_EMBED 模式（行格式）：
  arrays[i]    → 第 i 行的嵌入向量  MustDbVector{FLOAT32, dim, f32*}
  payloads[i]  → 第 i 行的 HEAP 用户列  RowVal[user_cols]
  col_rows[i]  → 第 i 行的标量列   RowVal[ncols]

MUSTDBCHUNK_COLUMN 模式（列格式）：
  arrays[i]    → 第 i 列的所有行值  MustDbVector{TypeID, nrows, data*}
```

**read_chunk 字段归属**（引擎不修改 `*out_idx`，由 orchestrator 管理）：

| 引擎 | 写入字段 | 数据类型 |
|------|---------|---------|
| `TamColTable` | `out_chunk->col_rows[*out_idx]` | `RowVal[ncols]` |
| `TamHeapTable` | `out_chunk->payloads[*out_idx]` | `RowVal[user_cols]` |

`MustDbChunk` 中 heap/col 用户数据已改为 `const Datum**` + 独立 null bitmap
（`payload_nulls` / `col_nulls`），不再是旧 RowVal 数组直接共享。

`get(seq_idx)` 使用顺序索引，不是 ctid。heap 与 col 是两个不同地址空间
（heap_ctid ≠ col_seq_idx），无法统一用 ctid。`fill_out_vals` 通过 `read_chunk`
vtable 调度各引擎，再由 bridge 步骤将 MustDbChunk 压平为
`[COL scalars..., HEAP user cols...]`（向量 bytes 由上层 EmbeddingStore 读取）。

### 批量插入上下文（TamInsertCtx）

```c
typedef struct {
    Transaction*  txn;            /* 必须是活动事务 */
    u32           cid;
    ItemPtr*       out_heap_ctids; /* [count] — heap 引擎插入后填充 */
    const ItemPtr* heap_ctids;     /* 调用方提供（NULL = 自动分配） */
    usize      count;
} TamInsertCtx;
// 引擎按 insert_phase 调度；当前 HEAP 填充 out_heap_ctids[]，POST_HEAP 可读取。
```

### MVCC / 事务（对齐 PostgreSQL）

事务系统由 `TransactionManager` + `Transaction` + `CommitLog` + `MvccSnapshot` 组成：

- `TxnId` 为 64 位单调递增 XID；`INVALID_TXN_ID=0`、`FROZEN_TXN_ID=1`、
  `BOOTSTRAP_TXN_ID=2`、首个用户 XID 为 `MIN_NORMAL_TXN_ID=3`。
- `CommitLog`（CLOG）：2 bit/XID 的内存数组，状态为 `IN_PROGRESS/COMMITTED/ABORTED`；
  由 `TransactionManager.clog` 持有。
- `MvccSnapshot{xmin,xmax,active_xids[],clog}`：Snapshot Isolation 快照；
  可见性规则优先看 `xmin/xmax/active_xids`，再回落 CLOG。
- `TransactionManager` 持有 `next_xid`、active XID 表、CLOG、WAL、checkpoint lock。
- `Transaction` 是显式事务句柄，携带 xid、snapshot、子事务状态栈、savepoint、
  ResourceOwner；底层 DML 必须接收活动 `Transaction*`。应用使用
  `mustdb_transaction_begin/commit/rollback`，不提供伪 backend-global 的 PG 命名薄封装。
- `TupleHdr` 不再携带 `infomask/infomask2/hoff`，可见性完全由 xmin/xmax + CLOG 判定。

#### INSERT / UPDATE / DELETE

heap 的公开 DML 走 PG heapam 风格 API（`heap_insert/heap_update/heap_delete`），
由 `StorageTable` 层通过 `TAM_*` vtable 转发；物理数据只写 header，列数据原地不动。

- **INSERT**：`t_xmin=xid, t_xmax=INVALID_TXN_ID(0), t_ctid=self`。
- **UPDATE**（MVCC append-only）：新版本 append，旧版本原地写 `t_xmax=xid`，
  `t_ctid` 指向新版本（forward pointer）；最新版本 `t_ctid` 自指。
- **DELETE**：`heap_delete(store, tid, txn, cid)` 只写 `t_xmax=txn->xid`，`t_ctid` 保持自指。
- **可见性**：`heap_fetch` / `heap_scan_iter_next` 通过 snapshot + CLOG 判定；
  自己事务的未提交 insert（`xmin==own_xid && xmax==0`）始终可见。
- **写冲突**：`heap_update_with_options` 与 `storage_table_delete_row` 返回
  `RsWriteResult`：`OK/DELETED/BEING_MODIFIED/NOT_FOUND/SELF_MODIFIED/UPDATED/INVISIBLE`。
- **ANN 可见性**：heap header + snapshot/CLOG 是唯一权威，索引命中必须回表或走绑定的
  visibility callback；不维护第二套 ANN MVCC 状态。

#### PG heapam 后续优化清单（先保存，后续按需做）

当前 `heap_insert` / `heap_update` / `heap_delete` / `heap_fetch` 已对齐 PG heapam
的核心语义骨架，但不是逐行复刻完整
PostgreSQL。下面能力先作为后续优化，不要混入当前 heap 主入口对齐任务。

| PG 能力 | 作用 | 嵌入式需要 | MustDB 建议优先级 |
|---------|------|------------|-------------------|
| HOT（Heap-Only Tuple） | UPDATE 未改索引列时尽量不更新二级索引，减少 index bloat | 需要但可后置 | P2/P3；等 btree/vector/fulltext index maintenance 稳定后做简化版 |
| MultiXact | 一个 tuple 的 `xmax` 表示多个事务共享锁/更新意图 | 低优先级 | P4；除非要做 SQL 外键、`SELECT FOR SHARE/KEY SHARE` 或复杂行级共享锁 |
| freeze + CLOG truncate | 冻结老 tuple XID 并截断旧事务状态，避免长期运行 CLOG 无限增长 | 需要 | ✅ 已完成：`txnmgr_vacuum_freeze_heap` + VM all-frozen + `clog_truncate` checkpoint/recovery；见 `test_heap_freeze_clog_maintenance` / `test_freeze` / `test_clog_truncate` |
| tuple lock wait policy | 遇到并发修改时支持 wait / nowait / skip locked / error | 部分需要 | ✅ 简化版已完成：`RsWriteOptions` + `CollectionWriteOptions`，支持 NOWAIT/WAIT/TIMEOUT/SKIP_LOCKED；见 `test_heap_tuple_wait_policy` / `test_collection_write_wait_policy` / `test_write_conflict` |
| combo CID | 同一事务内区分 insert command id 与 delete/update command id | 低优先级 | P3/P4；只有复杂 SQL executor/多 command 可见性需要 |
| infomask / infomask2 | tuple header 状态位和 hint bits，缓存提交状态、锁状态、HOT 标记等 | 部分需要 | ✅ hint bits/visibility cache 已完成：外置 `RowTupleHintEntry` + `RowHintStats`，heap fetch/scan 复用 CLOG 终态；完整 PG infomask 不照搬。见 `test_heap_visibility_hint_cache` / `test_pg_heap_p1` |
| 完整 TOAST | 大 TEXT/BLOB/JSONB 外置、压缩、分块、独立 toast table/index | 需要简化版 | ✅ 简化版已完成：versioned `RowVarExternal` + checksum `RowOverflowPage` + RLE 压缩 + page-image WAL/redo + checkpoint/free-list + vacuum reclaim；不照搬 PG toast table/index |
| serializable conflict / SSI | 可串行化隔离级别的读写冲突跟踪 | 早期不需要 | P4/可不做；嵌入式记忆库先用 Snapshot Isolation / Read Committed |

优先路线：**简化 TOAST/overflow**、**明确写冲突策略**、**hint bits/visibility
cache**、**freeze + CLOG truncate** 已完成；等二级索引生命周期稳定后，再做
**简化 HOT**。MultiXact、combo CID、SSI 不进入近期目标。

#### Vacuum / GC

普通 VACUUM 只做页内 prune 和可复用空间维护，不移动 live tuple、不改 CTID：

- `vacuum_row_store(store, safe_xid)` 把满足 `t_xmax != 0 && t_xmax <= safe_xid` 的
  dead slot 标为 LP_UNUSED；CLOG 版本 `vacuum_row_store_with_clog` 会先确认删除事务
  已提交，避免回收 aborted DELETE 的 tuple。
- vacuum cleanup 在标 LP_UNUSED 前先通过 `RowVacuumCleanupFn` 发布 vector_id 回收身份
  （先回收 EmbeddingStore/ANN 二级资源，
  再清 heap）。
- dead tuple 的内部 `_vector_id` 属性是二级资源清理键；heap tuple header 不保存
  embedding 物理地址。
- `EmbeddingStore` 生命周期由 heap xmax 决定；`free_list` 在水位线内复用单个向量槽，
  mmap 后端还通过 `EmbeddingPageRef.live` + retired extent 做整页/extent 回收。
- `VACUUM FULL` 走 `collection_vacuum_full`：扫描 live tuple → 重写新 `.mustdb` →
  重建所有索引 → 替换旧文件，旧 CTID 语义失效。

#### RowStore 页面布局（对齐 PG slotted page）

RowStore 使用 16 字节页头 + 4 字节 line pointer 的 PG 风格 slotted page。
`RS_PAGE_SIZE = BLOCK_SIZE - FILE_BUFFER_HEADER_SIZE`（8184B，Block buffer 尾部保留校验头）。

```
offset 0
┌───────────────────────────────────────────────────────┐
│ pd_lower(u16) pd_upper(u16) pd_flags(u16)              │  ← RS_BLOCK_HDR_SIZE = 16B
│ pd_pagesize_version(u16) pd_lsn(u64)                   │  RowPageHeaderData（含页 LSN）
├───────────────────────────────────────────────────────┤
│ RowItemId[0]: lp_off(u16) lp_len:14 lp_flags:2         │  ← RS_SLOT_SIZE = 4B
│ RowItemId[1]: ...                                      │  slot 数组向下增长
│ ...                                                    │
├───────────────────────────────────────────────────────┤
│ free space                                             │  = pd_upper - pd_lower
├───────────────────────────────────────────────────────┤
│ tuple data（新 tuple 从低地址写起，向页尾增长）         │
│ ...                                                    │
└───────────────────────────────────────────────────────┘
offset RS_PAGE_SIZE (8184)
```

常量：`RS_BLOCK_HDR_SIZE=16`，`RS_SLOT_SIZE=4`，
`RS_MAX_TUPLE_SIZE = RS_PAGE_SIZE - 16 - 4 = 8164B`。

Line pointer 状态用 `lp_flags` 的 2 bit 表达：`LP_UNUSED / LP_NORMAL / LP_REDIRECT / LP_DEAD`；
LP_UNUSED 槽保留在 slot 数组里不从数组中移除。`RowPageRepairFragmentation` 对齐 PG
`PageRepairFragmentation`，`RowPageVacuumDead` 把 LP_DEAD 转 LP_UNUSED。

**relation-local 页号**：CTID 的 `block_id` 直接等于 MAIN fork-local block number。
`RowRelation.pages` 只缓存可从 MAIN page 重建的 `start_seq / high_water / lp_count /
reusable_count`，不保存 global page id 或第二套物理页目录。单文件 extent 搬迁不改变 local block。

**物理 ctid 编码**（对齐 PG `ItemPointerData`，6B）：
```
ip_blkid_hi(u16) | ip_blkid_lo(u16) = block_id（relation MAIN fork-local block）
ip_posid(u16)                        = 1-based slot 号（PG OffsetNumber）
packed u64 = (block_id << 16) | ip_posid
seq_idx = heap_ctid_to_seqidx(store, ctid)   // 按 RowPageDesc.start_seq 映射
```

**on-disk tuple 格式**（每个 slot 内，无独立 row_id 字段）：
```
[TupleHdr (34B)][serialized col_0][col_1]...
```

**TupleHdr（34B，packed）**：

| offset | size | 字段 | 含义 |
|--------|------|------|------|
| 0 | 8 | `t_xmin` | 插入事务 XID（64 位） |
| 8 | 8 | `t_xmax` | 删除/更新事务 XID（0 = 存活） |
| 16 | 8 | `null_bits` | bit i=1 → 列 i 为 NULL（最多 64 列） |
| 24 | 4 | `t_cid` | Command ID |
| 28 | 6 | `t_ctid` | self/forward CTID，对齐 PG `HeapTupleHeaderData.t_ctid` |

与 PG 的差异：`t_xmin/t_xmax` 为 64 位；`null_bits` 固定 u64（PG 用可变 `t_bits[]`）；
没有 `infomask/infomask2/hoff`，可见性由 snapshot + CLOG 判定；向量身份不进入 header。

#### RowStore 插入与空间复用

- `heap_insert(store, txn, cid, values, null_bits)` 是主插入入口，
  内部按 PG `RelationGetBufferForTuple` 思路：FSM/hint 找有空间的逻辑页 →
  BufferPool pin → `RowPageAddItem` 写入 slot。
- `RowFreeSpaceMap`（简化 FSM，`Vector<RowFsmEntry>`）维护 page free-space hint，
  对应 PG `GetPageWithFreeSpace()`；`hint_free_block` 缓存最近可用页。
- 新页扩展由 `extend_lock` 串行化（对齐 PG `LockRelationForExtension`）。
- 插入优先复用 LP_UNUSED 槽；空间不足时先 `RowPageRepairFragmentation` 整理碎片，
  仍不足才扩展新页。
- 超长 varlena（TEXT/BYTEA/JSONB）走 `RowVarExternal` + `RowOverflowPage` 链
  （SQLite overflow page + PG TOAST pointer 思想的最小版）。

#### EmbeddingStore

`EmbeddingStore` 是独立的 f32 向量 bytes 存储，不挂在 `StorageTable` 引擎数组里：

- 后端：`EMBEDDING_STORE_BACKEND_MEMORY`（纯内存页）或
  `EMBEDDING_STORE_BACKEND_STORAGE_MMAP`（`MustDbStorageManager` 的 EMBEDDING fork + mmap extent）。
- `embedding_store_append` 返回 `ItemPtr emb_ctid`，优先复用 `free_list` slot，
  否则顺序 append；`embedding_store_write_at` 支持按 ctid 原地覆写。
- mmap 后端维护 `EmbeddingPageRef{block_id,slot,page_count,byte_offset,byte_len,live}`，
  `free_list` 复用单 slot；整页/extent 回收走 retired extent + reader_count 引用计数。
- `embedding_store_reader_enter/exit` 保护 reader 生命周期，
  `embedding_store_reclaim_retired` 归还 StorageManager free map。

### 向量索引（ANN / ViIndex LSM）

新 ANN 核心是 `ViIndex`（LSM 运行时），不再是一组各自独立实现 `VectorIndex` vtable
的 HNSW/IVF/Flat/DiskANN 对象：

- `ViIndex`：一个 mutable segment + 一组 immutable segment，LSM 风格。
  - 写入先进 mutable segment，达到阈值后 `vi_index_flush_mutable` 转成 immutable segment；
  - delete 记录为 `ViManifestTombstone`（id+seq），搜索时通过可见性回调过滤；
  - compact 合并 segment 并把旧 segment 标 obsolete，随后回收旧 extent。
- `ViManifest`：segment 列表（ACTIVE/OBSOLETE）、tombstone、dim/索引参数；
  通过 `MustDbStorageManager` 的 `VECTOR_MANIFEST` fork 持久化。
- `ViSegment` 种类：`VI_SEGMENT_FLAT / VI_SEGMENT_IVFFLAT / VI_SEGMENT_DISKANN / VI_SEGMENT_CAGRA`。
- segment 内向量 bytes 可内联（PLAIN），也可只存 `emb_ctid`（LSM/SBQ），
  搜索/rescore 时经 `ViVectorReadFn` 回读 `EmbeddingStore`。
- `ViSegmentReader` 后端：`RESIDENT / MMAP / PREAD_CACHE / WINDOW_MMAP / chunked mmap`；
  `ViSegmentCache` 提供 LRU/adaptive 缓存；`ViIoBackend` 支持
  `SYNC_PREAD / PTHREAD_PREAD / IO_URING` 批量读。
- DiskANN 算法支持 PLAIN 与 SBQ（`VaSbqQuantizer` 每维 bit 量化，默认 1 bit）
  两种存储模式，并提供流式搜索 state；标签过滤用 `ViLabelSet`。

#### 向量检索参考路线：zvec vs qdrant

MustDB 的嵌入式 `vector_index + embedding_store` 已经具备核心底座：向量 bytes 与
heap 解耦、`emb_ctid` 独立地址空间、EmbeddingStore mmap/free/retired extent、
ViIndex LSM segment、manifest/tombstone/compact 框架、Flat/IVF-Flat/DiskANN/SBQ、
segment reader/cache 与批量 IO 边界。

后续演进不要在 zvec 与 qdrant 之间二选一：

- **算法层优先对齐 zvec**：DiskANN/Vamana 构图、PQ、IVF int8/int4 量化、
  quantizer 训练/持久化、PQ distance table、beam/frontier search、sector/batch IO。
- **系统层优先对齐 qdrant**：payload filter AST、filter cardinality stats、
  prefilter/postfilter/exact planner、segment optimizer、deleted-ratio rebuild、
  snapshot validation、telemetry、memory budget。
- **嵌入式边界由 MustDB 自己决定**：所有设计必须落在单文件、StorageManager fork/extent、
  CTID/MVCC、sidecar WAL、插件化访问层上；不要照搬 qdrant 的分布式/shard/replica 架构，
  也不要绕过 MustDB 的 `EmbeddingStore`/`ViIndex`/`MustDbStorageManager` 边界。

当前缺口按优先级：

| 优先级 | 能力 | 参考 | 当前 MustDB 状态 |
|--------|------|------|------------------|
| P0 | PQ / Product Quantization | zvec DiskANN | ✅ 已完成：`va_pq.*` + DiskANN PQ segment storage，见 `test_va_pq` / `test_vi_diskann_pq_storage` |
| P0 | quantized coarse search + exact f32 rescore planner | zvec + MustDB EmbeddingStore | ✅ 已完成：`vi_search_planner.*` 路由 oversampling/exact fallback/rescore，见 `test_vi_search_planner` |
| P0 | quantizer 元数据持久化体系 | zvec IVF/DiskANN | ✅ 已完成 P0：`vi_quantizer.*` + manifest V7 descriptor；SBQ/PQ metadata 可 roundtrip，int8/int4 后续扩展 |
| P0 | payload filter planner | qdrant | ✅ 已完成 P0：`vi_filter.*` + `vi_payload_stats.*` 支持 label AST/cardinality/exact planner，见 `test_vi_filter_planner` |
| P0 | manifest 原子发布、checksum、crash recovery 验收 | qdrant snapshot 思路 + MustDB WAL | ✅ 已完成 P0：manifest tmp recovery/orphan cleanup + `vi_validation.*` CRC footer，见 `test_vi_validation` |
| P1 | int8/int4 scalar quantization | zvec IVF | ✅ 已完成 P1：`va_scalar_quant.*` 支持 int8/int4 train/encode/decode/L2/IP，见 `test_va_scalar_quant` |
| P1 | segment optimizer / deleted-ratio rebuild | qdrant optimizer | ✅ 已完成 P1：`vi_optimizer.*` 支持 L0 compact/deleted-ratio rebuild recommendation/apply，见 `test_vi_optimizer_policy` |
| P1 | 真实异步 IO（io_uring/libaio） | zvec DiskANN IO | ⚠️ P1 边界完成：`MUSTDB_ENABLE_IO_URING` compile-time option + requested/active/fallback stats；portable build 仍 fallback，真实 liburing fd pipeline 后续补 |
| P1 | recall/latency benchmark 与 ground truth | zvec/qdrant | ✅ 已完成 P2 基础验收：`test_vi_recall_ground_truth` 对比 Flat exact top-k，`bench_vector_retrieval` 提供延迟 smoke |
| P2 | fulltext + vector hybrid fusion | qdrant | ✅ 已完成 P2：`vi_fusion.*` 支持 RRF + exact vector rescore，见 `test_collection_hybrid_vector_fulltext` |
| P2 | visited pool 复用与搜索级 telemetry | qdrant | ✅ 已完成 P2：`vi_visited_pool.*` + `vi_telemetry.*` + runtime search telemetry，见 `test_vi_telemetry` |
| P3 | HNSW、sparse vector、multivector、GPU/CAGRA | qdrant | ✅ P3 无 GPU 子集已完成：`va_hnsw.*`、`va_sparse.*`、`vi_sparse_segment.*`、`va_multivector.*`、`vi_named_vector.*`、`va_rabitq.*`；GPU/CAGRA 仍预留 |

注意：`VI_SEGMENT_CAGRA` 当前只是 enum 预留；`io_uring` 当前是接口边界而不是真实
io_uring 实现；SBQ 不等同于 RabitQ。当前 RabitQ 是 P3 portable baseline，
不是 CUDA/SIMD 深度优化版。

#### 嵌入式向量检索已完成能力表

下面表格记录 `tmp/src/vector_index` 与 `tmp/src/embedding_store` 已经具备的向量检索
知识点。后续从缺口表完成一项后，必须把该项移动或补充到本表，并在状态中写明
对应核心文件/测试名。

| 类别 | 知识点 | 作用 | 嵌入式需要 | 当前状态 |
|------|--------|------|------------|----------|
| 存储 | EmbeddingStore 与 heap 解耦 | 向量 bytes 不塞 heap，heap 只管 MVCC 权威 | 需要 | ✅ 已完成：`EmbeddingStore` 独立持有 f32 bytes，heap 通过 `emb_ctid` 引用 |
| 存储 | `emb_ctid` 独立地址空间 | heap CTID 与 embedding CTID 解耦，避免顺序 iid | 需要 | ✅ 已完成：`ItemPtr emb_ctid` / `itemptr_pack` |
| 存储 | mmap 向量存储 | 大向量零拷贝读取，降低复制和常驻内存 | 需要 | ✅ 已完成：`EMBEDDING_STORE_BACKEND_STORAGE_MMAP` |
| 存储 | embedding free list | 删除后复用单个向量槽，控制文件增长 | 需要 | ✅ 已完成：`embedding_store_free` / `free_list` |
| 存储 | retired extent + reader_count | reader 退出后再回收 mmap extent | 需要 | ✅ 已完成：`embedding_store_reader_enter/exit` / `embedding_store_reclaim_retired` |
| 存储 | vector bytes 外置于 ANN node | DiskANN node 不内嵌原始 f32，节点只保留向量引用 | 需要 | ✅ 已完成：segment 可通过 `emb_ctid` 与 `ViVectorReadFn` 回读 |
| LSM | mutable/immutable segment | 写入先进 mutable，flush 后成为 immutable segment | 需要 | ✅ 已完成：`ViIndex.mutable_segment` / `vi_index_flush_mutable` |
| LSM | manifest ACTIVE/OBSOLETE | segment 发布、废弃、恢复入口 | 需要 | ✅ 已完成：`ViManifestSegmentState` |
| LSM | tombstone delete | 删除不原地改旧 segment，搜索时按 seq/id 过滤 | 需要 | ✅ 已完成：`ViManifestTombstone` / `vi_index_delete` |
| LSM | compact obsolete segment 基础 API | 合并 segment，把旧 segment 标 obsolete | 需要 | ✅ 已完成基础：`vi_index_compact_all` / `vi_index_compact_level` |
| 算法 | Flat exact segment | 小数据、回退路径、ground truth 基础 | 需要 | ✅ 已完成：`VI_SEGMENT_FLAT` |
| 算法 | IVF-Flat segment | coarse bucket 降低扫描量 | 需要 | ✅ 已完成：`VI_SEGMENT_IVFFLAT` |
| 算法 | DiskANN/Vamana segment | 大规模磁盘 ANN 主力路径 | 需要 | ✅ 已完成：`VI_SEGMENT_DISKANN` |
| 量化 | SBQ / binary quantization | 极低内存粗排，降低向量读 IO | 需要 | ✅ 已完成：`VaSbqQuantizer`，支持多 bit/dim |
| 量化 | SBQ 元数据与 codes 持久化 | reopen 后 SBQ 搜索行为一致 | 需要 | ✅ 已完成：SBQ `mean/m2/codes/num_bits` 已读写 |
| 搜索执行 | beam/list_size/io_limit 参数 | 控制 DiskANN recall、延迟和读放大 | 需要 | ✅ 已完成：`diskann_search_list_size` / `diskann_io_limit` 等参数边界 |
| 搜索执行 | streaming search state | segment 搜索可逐步产出候选，服务多段合并 | 需要 | ✅ 已完成：`ViSegmentSearchStream` / `VaDiskAnnSearchState` |
| 搜索执行 | `ViVectorReadFn` 原始向量回读 | ANN segment 只存引用时可回读 EmbeddingStore 做精排 | 需要 | ✅ 已完成接口：`vi_index_bind_vector_reader` |
| 过滤 | label filter | ANN 搜索时按轻量标签过滤 | 需要 | ✅ 已完成：`ViLabelSet` / `vi_segment_row_matches_labels` |
| IO | segment reader backing | 支持 resident/mmap/pread cache/window mmap/chunked mmap | 需要 | ✅ 已完成：`ViSegmentReader` 多后端 |
| IO | batch read 边界 | 减少随机读调用开销 | 需要 | ✅ 已完成基础：`vi_segment_reader_read_batch` / `ViIoBackend` |
| IO | pthread pread backend | 批量读可用线程并发化 | 建议 | ✅ 已完成基础：`VI_IO_BACKEND_PTHREAD_PREAD` |
| 缓存 | segment cache LRU/adaptive | 热 segment/page 缓存，降低重复 IO | 需要 | ✅ 已完成：`ViSegmentCache` |
| 缓存 | cache populate/clear 接口 | 支持显式预热和清理缓存 | 建议 | ✅ 已完成接口：`vi_index_populate_cache` / `vi_index_clear_cache` |
| 持久化 | segment magic/version | segment 格式自描述并严格拒绝非当前格式 | 需要 | ✅ 已完成：当前只接受 `VISEG105`，见 `test_current_format_fence` |
| 持久化 | StorageManager fork/extent 存储 | vector segment/manifest 进入单文件 fork | 需要 | ✅ 已完成基础：`VECTOR_SEGMENT` / `VECTOR_MANIFEST` fork |
| 统计 | reader/cache memory stats | 观察 resident/mapped/cached/pin/pread 等读路径状态 | 需要 | ✅ 已完成基础：`ViIndexReaderStats` / `ViSegmentReaderStats` |
| 架构 | `vector_algo` 可复用边界 | 算法层不反向依赖 `vector_index`/StorageManager/CTID | 需要 | ✅ 已完成：`va_filter.*`、`va_segment_view.h`、`vi_algo_adapter.*`，见 `test_va_algo_boundary` / `test_va_diskann_algo_boundary` |
| 量化 | PQ / Product Quantization | DiskANN PQ 粗排压缩，降低 IO 与内存 | 需要 | ✅ 已完成：`va_pq.*`、`VA_DISKANN_STORAGE_PQ`、DiskANN V13 layout PQ codebook/codes，见 `test_va_pq` / `test_vi_diskann_pq_storage` |
| 量化 | quantizer descriptor serialization boundary | 统一描述 SBQ/PQ/int8/int4 量化元数据 | 需要 | ✅ 已完成：`vi_quantizer.*`、`ViManifest.quantizer`，见 `test_vi_quantizer_io` / `test_vector_index_manifest` |
| 搜索执行 | unified search planner | exact fallback、oversampling、rescore budget 统一入口 | 需要 | ✅ 已完成：`vi_search_planner.*` + `vi_index_search_with_params` 路由，见 `test_vi_search_planner` |
| 过滤 | payload filter AST/cardinality planner | `must/should/must_not/min_should` 与过滤选择性估算 | 需要 | ✅ 已完成 P0：`vi_filter.*`、`vi_payload_stats.*`，见 `test_vi_filter_planner` |
| 持久化 | vector validation/recovery hardening | checksum footer、manifest tmp recovery、orphan cleanup 验收 | 需要 | ✅ 已完成 P0：`vi_validation.*` + `vi_recovery.c` 现有 cleanup 验收，见 `test_vi_validation` |
| 量化 | int8/int4 scalar quantization | IVF/Flat 粗排压缩，降低内存 | 建议 | ✅ 已完成 P1：`va_scalar_quant.*` 提供 per-dimension min/scale train、int8/int4 encode/decode、approx L2/IP，见 `test_va_scalar_quant` |
| 生命周期 | segment optimizer policy | 控制 L0 fanout 和 deleted-ratio rebuild | 需要 | ✅ 已完成 P1：`vi_optimizer.*` 提供 `vi_index_optimizer_recommend/apply`，见 `test_vi_optimizer_policy` |
| 生命周期 | memory budget / admission control | 控制 cache/rescore/build/train 预算 | 需要 | ✅ 已完成 P1：`vi_memory_budget.*` + `ViSegmentCache` admission + `ViIndex` rescore clamp，见 `test_vi_memory_budget` |
| IO | io_uring compile-time boundary | 保留真实异步 IO 接入口且默认 portable fallback | 建议 | ✅ 已完成 P1 边界：`MUSTDB_ENABLE_IO_URING`、requested/active/fallback stats，见 `test_vi_io_backend_fallback` / `test_vi_io_backend_batch` |
| 搜索执行 | visited pool 复用 | 避免每次 ANN 搜索重复分配 visited bitmap | 建议 | ✅ 已完成 P2 基础：`vi_visited_pool.*` 提供 reusable bitmap pool，见 `test_vi_telemetry` |
| IO | cache warmup policy | reopen 后按策略预热 metadata/entrypoints/hot windows/full-small | 建议 | ✅ 已完成 P2：`vi_index_warmup_cache()` + `ViCacheWarmupPolicy`，见 `test_vi_cache_warmup_policy` |
| 混合检索 | fulltext + vector fusion | BM25/FTS ranked list 与 ANN ranked list 融合 | 建议 | ✅ 已完成 P2：`vi_fusion.*` 支持 RRF 和 exact rescore，见 `test_collection_hybrid_vector_fulltext` |
| 可观测性 | search telemetry | 搜索访问候选、filter、rescore、cache、elapsed 调参数据 | 需要 | ✅ 已完成 P2：`ViSearchTelemetry` + `vi_index_set_search_telemetry()`，见 `test_vi_telemetry` |
| 质量 | recall/latency regression tests | 防止优化后 recall 或延迟退化 | 需要 | ✅ 已完成 P2 基础：`test_vi_recall_ground_truth` / `bench_vector_retrieval` |
| 算法 | HNSW online graph | 在线增量图索引原语，作为可复用 ANN 补充 | 可选 | ✅ 已完成 P3 基础：`va_hnsw.*` 支持 online insert/search/delete mark，见 `test_va_hnsw` |
| 量化 | rotation / FHT hook | 为 OPQ/RabitQ 提供可复用旋转基础 | 建议 | ✅ 已完成 P3 基础：`va_rotate.*` 支持 sign rotation、FHT/inverse，见 `test_va_rotate` |
| 量化 | RabitQ portable baseline | binary quantization 粗排基础路径，含元数据和 code roundtrip | 建议 | ✅ 已完成 P3 基础：`va_rabitq.*` + `VI_QUANTIZER_RABITQ`，见 `test_va_rabitq` |
| 混合检索 | sparse vector primitives/index | SPLADE/稀疏 embedding 的 dot/cosine/top-k 与轻量 segment | 部分需要 | ✅ 已完成 P3 基础：`va_sparse.*`、`vi_sparse_segment.*`，见 `test_va_sparse` / `test_vi_sparse_segment` |
| 多向量 | named vectors / multivector | 多字段 embedding registry 与 ColBERT 类 MaxSim scorer | 可选 | ✅ 已完成 P3 基础：`vi_named_vector.*`、`va_multivector.*`，见 `test_vi_named_vectors` / `test_va_multivector` |
| IO | direct IO alignment policy | 为未来 O_DIRECT/libaio/io_uring 批量读提供 sector 对齐规划 | 建议 | ✅ 已完成 P4 policy 层：`vi_direct_io.*`，见 `test_vi_direct_io_policy`；默认仍是 buffered IO |
| 搜索执行 | batch multi-query search | 多 query 共用 prefetch/cache 后逐 query 保持单查语义 | 建议 | ✅ 已完成 P4：`vi_batch_search.*`，见 `test_vi_batch_search` |
| 持久化 | current-format fence | DB/WAL/vector/fulltext 只读取当前格式，避免多代在线分支 | 需要 | ✅ 已完成：DB v7、WAL v2、`VISFDB05`、`VISEG105`、`VIDSAN13`、IVF manifest v3、FTA v2；见 `test_current_format_fence` |
| 持久化 | snapshot archive validation | 检查 segment/state/seq 高水位一致性 | 需要 | ✅ 已完成 P4 基础：`vi_snapshot_validate.*`，见 `test_vi_snapshot_validate` |
| 并发 | reader snapshot pin | 搜索期间用 generation/refcount 推迟 obsolete reclaim | 需要 | ✅ 已完成 P4 基础：`vi_reader_snapshot.*` + compaction generation bump，见 `test_vi_reader_snapshot` |
| 可观测性 | build/compact telemetry | segment build、IVF assignment、compact/rebuild 边界计数 | 建议 | ✅ 已完成 P4：`ViBuildTelemetry` / `ViCompactTelemetry`，见 `test_vi_build_compact_telemetry` |

#### 嵌入式向量检索缺口跟踪表

下面表格是 `tmp/src/vector_index` 与 `tmp/src/embedding_store` 后续硬化清单。每实现好
一项，必须同步更新本表状态，把 `❌ 未做` 或 `⚠️ 部分` 改成 `✅ 已完成`，并把完成项
补充到上面的已完成能力表，状态中写明对应核心文件/测试名。

| 类别 | 知识点 | 作用 | 嵌入式需要 | 当前状态 |
|------|--------|------|------------|----------|
| 量化 | PQ / Product Quantization | DiskANN 磁盘 ANN 的核心压缩与粗排，降低 IO | 需要 | ✅ 已完成：`va_pq.*` + DiskANN PQ segment storage/reopen/search，见 `test_va_pq` / `test_vi_diskann_pq_storage` |
| 量化 | OPQ / random rotation | PQ/SBQ 前旋转，降低量化误差 | 建议 | ⚠️ P4 hook 完成：`va_rotate.*` + SBQ/PQ `_with_rotation` wrappers，见 `test_va_rotate`；完整 OPQ training 仍未做 |
| 量化 | int8/int4 scalar quantization | IVF/Flat 粗排压缩，降低内存 | 建议 | ✅ 已完成 P1：`va_scalar_quant.*` 支持 int8/int4 train/encode/decode/L2/IP，`ViIndexOptions.quantizer` 可规整 scalar code_size |
| 量化 | RabitQ / FHT | binary quantization 的高 recall 路线 | 建议 | ✅ P3 基础完成：`va_rabitq.*` + `va_rotate.*` 提供 train/encode/distance/serialization，见 `test_va_rabitq` / `test_va_rotate`；SIMD/FHT 深度优化后续单列 |
| 量化 | quantizer 训练采样策略 | 大数据不能全量训练，需要采样/限内存训练 | 需要 | ⚠️ 部分；SBQ 有在线统计，PQ 有确定性 sampled train，int8/int4 已有训练框架，`vi_memory_budget_training_rows()` 提供预算裁剪入口 |
| 量化 | quantizer 参数持久化 | reopen 后量化行为一致 | 需要 | ✅ 已完成 P0：`vi_quantizer.*` descriptor + manifest V7；PQ codebook/codes 在 DiskANN V13 layout 持久化 |
| 搜索执行 | quantized coarse + exact rescore | 量化粗排后回读 f32 精排，保证 recall | 需要 | ✅ 已完成：SBQ/PQ coarse + `ViVectorReadFn` exact rescore planner，见 `test_vi_diskann_pq_storage` / `test_vi_search_planner` |
| 搜索执行 | oversampling / rescore budget | 控制候选数、精排量、延迟 | 需要 | ✅ 已完成 P0：`vi_search_planner_choose()` 统一计算 ann/rescore budget |
| 搜索执行 | brute-force fallback planner | 小表/强过滤时 exact scan 比 ANN 更快 | 需要 | ✅ 已完成 P0：small index / strong filter exact fallback 路由到 `vi_index_search_exact` |
| 搜索执行 | visited pool 复用 | 避免每次 ANN 搜索重复分配 visited bitmap | 建议 | ✅ 已完成 P2 基础：`vi_visited_pool.*` reusable bitmap pool，后续可接入 DiskANN 内部 visited 分配路径 |
| 搜索执行 | batch multi-query search | 多 query 批量检索，共享 IO/cache | 建议 | ✅ 已完成 P4：`vi_index_search_batch()` / `vi_index_search_batch_with_stats()` 共享 prefetch 并保持单查结果语义 |
| 搜索执行 | recall ground truth benchmark | 用 exact top-k 对比 ANN recall@k | 需要 | ✅ 已完成 P2 基础：`test_vi_recall_ground_truth` 对 IVF/DiskANN tiny deterministic dataset 验收 recall@k，`bench_vector_retrieval` 提供延迟 smoke |
| 过滤 | payload filter AST | `must/should/must_not/min_should` 表达复杂过滤 | 需要 | ✅ 已完成 P0：`vi_filter.*` 支持 label leaf + AND/OR/NOT/MIN_SHOULD |
| 过滤 | payload index cardinality stats | 估算过滤选择性，决定 prefilter/postfilter/exact | 需要 | ✅ 已完成 P0：`vi_payload_stats.*` label cardinality estimate |
| 过滤 | filter-aware ANN planner | 过滤条件下保证 top-k 足量与性能 | 需要 | ✅ 已完成 P0：planner 使用 filter expected matches 选择 exact fallback |
| 过滤 | iterative search / ensure_topk_full | 过滤后候选不足时继续扩大搜索 | 需要 | ⚠️ 部分；有 `ensure_topk_full` 字段，策略需完善 |
| 生命周期 | background segment optimizer | 自动 flush/compact/vacuum，控制 segment fanout | 需要 | ✅ 已完成 P1 策略层：`vi_optimizer.*` 无后台线程版 recommendation/apply；真正 runtime 后台调度另属 runtime 计划 |
| 生命周期 | deleted-ratio rebuild | 删除比例过高时重建 segment，恢复 recall/空间 | 需要 | ✅ 已完成 P1：`vi_index_optimizer_recommend()` 根据 `deleted_count / row_count` 触发 `VI_OPTIMIZER_ACTION_REBUILD_DELETED` |
| 生命周期 | obsolete extent 回收验收 | compact 后旧 page range 进入 free map | 需要 | ⚠️ 部分；机制有，需 crash/pressure 测试 |
| 生命周期 | memory budget / admission control | 控制 mmap/cache/rescore/构建内存上限 | 需要 | ✅ 已完成 P1：`ViMemoryBudget` 覆盖 mapped/cache/rescore/build/train caps，cache 超预算拒绝，rescore 候选可降级 |
| 持久化 | manifest 原子发布 | flush/compact 崩溃时保持新旧 manifest 一致 | 需要 | ✅ 已完成 P0：`vi_manifest_save()` tmp+fsync+rename，`vi_index_recover_directory()` 清理 interrupted tmp，见 `test_vi_validation` |
| 持久化 | orphan segment cleanup | 崩溃后清理未发布或废弃 segment extent | 需要 | ✅ 已完成 P0：`vi_index_reclaim_orphans()` 验收未引用 segment 删除，见 `test_vi_validation` |
| 持久化 | vector segment checksum | 检测单文件拷贝损坏、mmap 读坏页 | 需要 | ✅ 已完成 P0：`vi_validation.*` CRC32 footer API，见 `test_vi_validation` |
| 持久化 | format migration / compatibility | segment 升级策略 | 需要 | ✅ 当前策略：实验树不在线兼容旧文件，非当前格式明确拒绝；迁移器已删除 |
| 持久化 | snapshot archive validation | 备份/迁移时验证 heap/embedding/vector 一致 | 需要 | ✅ 已完成 P4 基础：`vi_snapshot_validate.*` 检查 segment/state/seq 高水位一致性；跨 heap/embedding 归档校验后续接 Collection |
| IO | 真实 io_uring / libaio | DiskANN 随机读并发化，降低 tail latency | 建议 | ⚠️ P1 边界完成：`MUSTDB_ENABLE_IO_URING` + requested/active/fallback stats；portable build fallback 到 sync，真实 liburing fd pipeline 后续结合 direct IO/reader fd API 实现 |
| IO | direct IO alignment policy | 避免 page cache 干扰，匹配 DiskANN sector/page 读 | 建议 | ✅ 已完成 P4 policy 层：`vi_direct_io_plan_read()` 计算 aligned range/copy window；不默认启用 O_DIRECT |
| IO | cache warmup/populate policy | reopen 后预热热点 segment/page | 建议 | ✅ 已完成 P2：`vi_index_warmup_cache()` 支持 none/metadata/entrypoints/hot windows/full-small |
| 混合检索 | fulltext + vector fusion | 文档场景常用 BM25 + ANN + rerank | 建议 | ✅ 已完成 P2：`vi_fusion_rrf()` 对 ranked lists 做 RRF，`vi_fusion_apply_exact_rescore()` 支持精排 |
| 混合检索 | sparse vector index | SPLADE/稀疏 embedding 检索 | 部分需要 | ✅ P3 基础完成：`va_sparse.*` + `vi_sparse_segment.*` 支持 sparse dot/cosine/top-k、visibility filter、save/load |
| 多向量 | named vectors / multivector | 多字段 embedding、ColBERT 类 MaxSim | 可选 | ✅ P3 基础完成：`va_multivector.*` + `vi_named_vector.*` 支持 MaxSim、field registry、field-specific search |
| 可观测性 | search telemetry | IO、cache hit、filter hit、rescore、latency 调参 | 需要 | ✅ 已完成 P2：`ViSearchTelemetry` 统计 search_count/candidates/prefilter/rescore/cache/elapsed |
| 可观测性 | build/compact telemetry | 构建耗时、写放大、删除比例、回收空间 | 建议 | ✅ 已完成 P4：build rows/bytes/centroids/assignments 与 compact/rebuild segment counters 已接入 `ViIndex` |
| 并发 | reader snapshot pin | 搜索期间 segment/embedding 不被回收 | 需要 | ✅ 已完成 P4 基础：`ViReaderSnapshot` 记录 generation/segment_count，active reader 退出前禁止 obsolete reclaim |
| 质量 | recall/latency regression tests | 防止优化后 recall 或延迟退化 | 需要 | ✅ 已完成 P2 基础：deterministic recall test + latency smoke bench；大规模标准数据集后续单独做 |

#### 全文检索演进状态

`tmp/src/fulltext_algo` 是可复用全文检索算法库，`tmp/src/fulltext_index` 是 MustDB
segment/manifest/lifecycle 封装层。完成全文检索任务后必须同步更新本表。

| 阶段 | 能力 | 层级 | 当前状态 |
|------|------|------|----------|
| P0 | tokenizer/analyzer、varint/buffer、posting/doclist、vocab、immutable segment | `fulltext_algo` | ✅ 已完成：`fta_tokenizer.*` / `fta_analyzer.*` / `fta_doclist.*` / `fta_segment.*`，见 `test_fta_varint` / `test_fta_tokenizer` / `test_fta_doclist` / `test_fta_vocab` / `test_fta_segment` |
| P0/P1 | 严格 UTF-8 + CJK overlapping bigram、自然语言 OR/BM25 query、analyzer 版本化 | `fulltext_algo` → `fulltext_index` → Collection/Corvus | ✅ 已完成：CJK 连续文本生成 bigram、孤字 unigram，覆盖全角文本、半角片假名、Hangul Jamo 与扩展汉字，拒绝非法 UTF-8；`fta_query_parse_natural()` 使用有预算的平衡 OR tree，显式布尔语义不变；segment V2 持久化 analyzer 配置指纹并拒绝不兼容查询/merge，Corvus 默认 natural 模式。见 `test_fta_tokenizer` / `test_fta_query` / `test_fta_segment` / `test_fti_runtime` / `test_collection_fulltext_search` / `typed_repository_uses_natural_cjk_fulltext_query` |
| P1 | query AST、phrase/NEAR/prefix eval、BM25、stable top-k、segment merge | `fulltext_algo` | ✅ 已完成：`fta_query.*` / `fta_eval.*` / `fta_bm25.*` / `fta_topk.*` / `fta_merge.*`，见 `test_fta_query` / `test_fta_eval_bm25` / `test_fta_merge` |
| P2 | manifest ACTIVE/OBSOLETE、segment metadata、doc_id→heap CTID map、tombstone、flush/reopen | `fulltext_index` | ✅ 已完成：`fti_manifest.*` / `fti_segment.*` / `fti_runtime.*` / `fti_tombstone.*`，见 `test_fti_manifest` / `test_fti_runtime` |
| P2 | StorageManager FULLTEXT fork segment write/read ex API，保留旧 storage helper API | `fulltext_index` | ✅ 已完成：`fti_storage_store.*`，见 `test_fti_runtime` / `test_fts_single_file_store` / `test_fts_storage_manager_store` / `test_sfdb_segment_header` |
| P2 | qdrant-style compaction policy、deleted-ratio/fanout recommendation、compact 后 tombstoned doc 物理排除、obsolete reclaim | `fulltext_index` | ✅ 已完成：`fti_compaction.*` + `fti_index_compact()` / `fti_index_reclaim_obsolete()`，见 `test_fti_compaction` |
| P2 | validation、active segment open-time 校验、checksum/page_count 校验、orphan cleanup smoke | `fulltext_index` | ✅ 已完成：`fti_validation.*` + `fti_index_open()` validation，见 `test_fti_validation` |
| P2 | 全局 fulltext top-k ranking | `fulltext_index` | ✅ 已完成：runtime 搜索收集 mutable+immutable 候选并按 BM25 全局排序，见 `test_fti_runtime` |
| P3 | zvec-style bitpacked posting blocks | `fulltext_algo` | ✅ 已完成：`fta_bitpack.*` 支持 128-doc block、doc delta/tf/doc_len bitpack、block max score、next/advance、checksum/truncation 校验，并接入 `FTA_SEGMENT_FLAG_BITPACK_DOCLISTS`，见 `test_fta_bitpack` / `test_fta_segment` |
| P3 | WAND-style block pruning | `fulltext_algo` | ✅ 已完成：`fta_wand.*` 支持 force-exact parity、bitpack block max score pruning、top-k score/doc_id 稳定排序与 pruning stats，见 `test_fta_wand` |
| P3 | fulltext segment cache populate/clear 与搜索 telemetry | `fulltext_index` | ✅ 已完成：`fti_cache.*` + `fti_index_populate_cache()` / `fti_index_clear_cache()` / `fti_index_last_search_stats()`，统计 segments_scanned/postings_decoded/candidates_scored/tombstones_filtered/cache_hits/cache_misses/resident_bytes，见 `test_fti_runtime` |
| P4 | CTID visibility callback / MVCC boundary | `fulltext_index` | ✅ 已完成：`FtiVisibilityFn` + `fti_index_bind_visibility()` 在返回结果前过滤 heap CTID，heap/MVCC 仍是可见性权威；结果 flags 标记 callback check，见 `test_fti_runtime` |
| P4 | fulltext + vector hybrid bridge result shape | `fulltext_index` | ✅ 已完成：`fti_search_result.h` 固定 `FtiSearchResult{heap_ctid,score,segment_id,flags}`，并提供 `fti_search_results_pack()` 转 packed CTID + score arrays；u64 segment id 溢出时用 sentinel + `FTI_SEARCH_RESULT_FLAG_SEGMENT_ID_OVERFLOW` 显式标记，见 `test_fti_hybrid_bridge` |

#### Collection 检索能力开放状态

`Collection` 是用户侧公开 API，只负责配置、事务、可见性校验与访问层编排；不得越过
`DocumentAccess` 直接解析 `fulltext_index` segment 或 `vector_index` segment。

| 能力 | Collection 状态 |
|------|-----------------|
| `vector_index` 基础/高级配置 | ✅ 已开放：`CollectionVectorOptions` + 底层 `ViIndexOptions`，见 `test_collection_vector_config` |
| vector search/cache/compact/stats | ✅ 已开放：`collection_search` / `collection_vector_*` APIs，见 `test_collection_store` / `test_collection_vector_lsm_bridge` |
| fulltext explicit/natural query mode | ✅ 已开放：`CollectionFulltextSearchOptions.query_mode` 支持 `COLLECTION_FULLTEXT_QUERY_EXPLICIT/NATURAL`；`filter_fn` 在全局 top-k 截断前按可见 `DocumentRow` 过滤；默认显式语法兼容，Corvus 选择 natural OR/BM25 并提前过滤 scope/status/trust，见 `test_collection_fulltext_search` / `typed_repository_filters_scope_before_applying_result_limit` |
| fulltext field config | ✅ 已开放：`DOC_FIELD_INDEX_FULLTEXT` 标记 `DOC_FIELD_TEXT` 字段，见 `test_collection_fulltext_search` |
| fulltext search/cache/compact/stats | ✅ 已开放：`collection_fulltext_*` APIs，见 `test_collection_fulltext_search` |
| hybrid BM25 + ANN fusion | ✅ 已开放：`collection_hybrid_search` 通过 fulltext/vector ranked lists + `vi_fusion_rrf()` 融合，见 `test_collection_hybrid_search` |

注意：P3 bitpacked posting blocks 当前只保存 `doc_delta/tf/doc_len`，不保存真实 positions。
因此 bitpacked segment 可用于 term/prefix/WAND 粗排；phrase/NEAR 这类位置查询必须继续使用
varint doclist 或后续新增 positional sidecar。`fta_eval_query()` 对 bitpacked segment 的
phrase/NEAR 会显式返回 `FTA_EINVAL`，避免产生错误位置匹配。

向量运行时只有 `ViIndex` LSM 主路径。可复用算法类型使用 `external_ref`，MustDB
adapter 才把它解释为 packed embedding handle；不保留旧 `VectorIndex` vtable 或
DiskANN wrapper。

### 页面感知磁盘格式（index_page.h）

现在只保留 DiskANN 页面格式；HNSW/IVF 的独立 page magic 已移除，IVF-Flat 走
`ViSegment` 布局。所有页统一用 `MustDbPageHdr`（16B）：
`pd_lower/pd_upper/pd_flags/reserved/page_type/pd_lsn`。

| 页类型 | page_type | Page ID | Opaque | 对齐目标 |
|--------|-----------|---------|--------|---------|
| DiskANN meta/node | `0x06/0x07` | `0xA202` | 4B（无 next_block）| pgvectorscale |
| RowStore | `0x20` | — | 16B `RowPageHeaderData` | PG slotted page |
| EmbeddingStore | `0x21` | — | — | mmap extent / 顺序页 |

DiskANN 页面布局：

- meta 页放 `DiskANNMetaTuple`（64B：magic `0x44534E4E`、version、R/L/alpha、
  `default_node`、`total_nodes`、`first_node_block` 等）+ 若干 `DiskANNLabeledNode`。
- node 页是 slotted page：`MustDbSlot`（4B）槽 + `DiskANNNodeHdr`（32B，
  含 `heap_ctid`、算法中立的 `external_ref`、`xmax`）+ 固定 R 个 `NodePtr` 邻居 + i16 标签。
- 向量 bytes 不嵌入 node tuple，由 `EmbeddingStore` 持有。

### 二级索引 / Relation / 访问层

- `BTreeIndex`：PG nbtree 启发的最小 B+Tree，key → heap CTID；heap 仍是 MVCC 权威。
  支持 build/insert/delete/update/beginscan/gettuple，页为 slotted page +
  `BTreePageOpaqueData`；当前单列、前向扫描、无并发 split 恢复。
- `MustDbIndexAm`（`index_am.h`）：统一 BTree / Vector 的二级索引 vtable
  （insert/delete/scan/destroy），`index_am_create_btree` / `index_am_create_vector`。
- `MustDbRelation`（`relation.h`）：`StorageTable*` + relation-local embedding/index state，
  是 table/index AM 的打开句柄；`relation_insert/fetch/scan/vacuum/wal_redo`。
- `DocumentAccess` / `GraphAccess` 是 relation + index_am 之上的内部插件实现，
  不是公开 facade，也不重复拥有 relation 已持有的 EmbeddingStore。
- `MustDbAccess`（`access.h`）是插件化访问层：`MustDbAccessRoutine` vtable +
  `MustDbAccessManager` + catalog，支持 register type / create / call / vacuum。
- `Collection`（`collection.h`）是最上层公开 API，封装 open/close、事务、
  document/graph 操作、向量搜索、checkpoint、vacuum、vector compact、
  single-file tail truncate、vacuum_full 与维护统计。

#### 运行时源码职责边界

- **可迁移阅读顺序**：`mustdb_database_create/open` -> `mustdb_table_create/open` ->
  `MustDbRelation` -> `StorageTable/TableAM` -> `heap_insert/update/delete/fetch/scan` ->
  `RowPage/BufferPool` -> `page WAL/checkpoint` -> `MustDbStorageManager`。关键入口注释必须
  说明职责、所有权、失败回滚和 PG 对照；不得为了抽象增加无实际价值的 wrapper/context。
  函数级调用链以 `docs/architecture/current-prototype.md` 第 10 节为准。
- runtime `Catalog` 只注册和查找已经由正式 DDL 创建、打开的 `StorageTable`，并承担
  bootstrap、relation cache 与定向 WAL redo 路由；不得再提供绕过系统表的建表工厂。
- 表 DDL 统一走 `mustdb_table_create()`，Schema/Table/Column 逻辑元数据以系统 relation
  为权威；index DDL/open 统一走 `MustDbRelation` + `MustDbIndexAm`，并持久化到
  `mustdb_sys_index`。数据库 reopen 扫描系统表恢复 relation，再从 heap 对每个派生索引
  恰好构建一次；不得维护第二套 `IndexCatalog` 或 cache-only index 生命周期。
- Collection 内置 document/graph 路径直接调用 `document_access_*` / `graph_access_*`
  typed internal API；`MustDbAccessRoutine.call` / `mustdb_access_call()` 只保留为外部插件
  ABI 和插件契约测试边界，不得作为内置 Collection 的内部调度器。
- `DocumentAccess` 对 Collection 保持不透明。具体结构只允许出现在
  `document_access_private.h`；`collection*.c` 和 `single_file_vacuum.c` 只能调用
  `document_access_*` typed operations，不得出现 `c->docs->...`，也不得为内置 docs
  路径构造 `DocumentAccess*Args` 后再走通用 plugin call。通用
  `MustDbAccessRoutine.call` 仅保留给插件调用者和插件契约测试。
- `document_access.c` 只负责 create/destroy 和轻量 accessor；row CRUD、全文、向量、
  rebuild/recovery 分别归 `document_access_row.c`、`document_access_fulltext.c`、
  `document_access_vector.c`、`document_access_recovery.c`。跨文件实现 helper 必须使用
  `document_access_private_*` 前缀。
- Collection 的配置复制与 create/open 装配归 `collection_open.c`，live handle lease、
  close/drain 和最终销毁归 `collection_lifecycle.c`，WAL/reopen 编排归
  `collection_recovery.c`，checkpoint participant 与 vector/fulltext 发布归
  `collection_checkpoint.c`。不得再把 recovery/index 实现塞回 lifecycle。
- RowStore 按 PG heap 责任拆分：`row_store.c` 对应 relation 生命周期与状态装配，
  `row_store_page_io.c` 对应 `hio.c`/buffer-page 协调，`row_store_tuple.c` 对应
  `heaptuple.c`，`row_store_heap.c` 对应 `heapam.c` 的 DML/fetch/scan，
  `row_store_visibility.c` 对应 `heapam_visibility.c`，`row_store_vacuum.c` 对应
  `pruneheap.c`/`vacuumlazy.c`，`row_store_wal.c` 对应 `heapam_xlog.c`。
  这是嵌入式语义对齐，不引入 PG backend 全局状态。故障注入接口只放在
  `row_store_test.h`，不得进入 `row_store.h` 公共 heap AM 契约。

### WAL / 恢复

- `WAL` 记录：20B `WalRecordHdr{total_len,crc32,xid,type,_pad}` + payload。
- WAL 主路径固定为 `append -> 256KB userspace ring -> batch pwrite -> fsync`；
  `base_lsn <= flush_lsn <= write_lsn <= insert_lsn`。所有 append API 返回 record
  end-LSN，page LSN 与 commit LSN 都使用该 end-LSN；只有 `wal_flush_upto()` 能推进
  durable 的 `flush_lsn`，调用方不得直接访问 WAL 内部边界或自行 `fsync(wal->fd)`。
- 类型含 `WAL_HEAP_INSERT/DELETE`、`WAL_VECTOR_PUT/DELETE/INTENT/INTENT_STATE`、
  `WAL_COL_INSERT`、通用 `WAL_PAGE_IMAGE(S)`、`WAL_TXN_BEGIN/COMMIT/ABORT/CHECKPOINT`。
- 页镜像 WAL 通过 `rel_id + fork + block_id + page_lsn` 分发，
  `MustDbPageRedoRegistry` 只负责 redo 路由；数据库级 BufferPool 负责统一刷脏。
- `mustdb_checkpoint` 对齐 PG `CreateCheckPoint/CheckPointBuffers/BufferSync`：冻结本轮 dirty
  generation，全局扫描 BufferPool（系统表与用户表一视同仁），同步 direct-I/O participant
  和 SMGR fork/free map，写并 flush checkpoint WAL，轮转双 DBHeader，最后回收 WAL 前缀。
- checkpoint control 只保存物理恢复边界，不遍历或复制六张系统表；系统 catalog relation
  是 schema/table/column/index/access/sequence 的唯一逻辑权威。实验树只接受当前格式，
  不保留 v5 relation-copy 或在线/离线迁移器。
- 双 DBHeader 使用固定小端 codec、checksum、generation；当前 v7 generation 不完整时
  只回退同格式的上一完整 generation。`txnmgr_recover` 从 sidecar WAL 重建 CLOG/next_xid。
- sidecar WAL 由 `single_file_wal_path/init_sidecar/checkpoint_delete` 管理，
  checkpoint 成功后可以删除/截断。

## 命名约定

| 概念 | 正确命名 | 禁用命名 |
|------|---------|---------|
| ANN 算法节点的外部引用 | `external_ref: u64` | ~~emb_iid~~ |
| EmbeddingStore 追加/计数 | `embedding_store_append` / `embedding_store_count` | ~~append_and_get_ctid~~ |
| 批量插入上下文 heap 输出 | `out_heap_ctids` | ~~out_row_ids / iids~~ |
| MustDB 搜索结果行位置 | `heap_ctid` / `heap_ctid_packed` | ~~internal_id / emb_ctid_packed~~ |
| ANN 旧槽索引 | `old_slot` | ~~old_iid~~ |
| ColumnStore 顺序索引 | `seq_idx` | ~~iid~~ |
| RowStore 槽计数 | `heap_relation_slot_count()` | ~~row_store_slot_count / next_internal_id~~ |

## 编码规范

- **类型**：使用 `u8/u16/u32/u64/i32/i64/f32/f64/usize`，不用 `int/long/size_t`
- **虚方法**：`VCALL(obj, method, args...)` — 不直接调用函数指针
- **内存**：手动管理，无 GC。`malloc/free` 显式配对；`TAM_DESTROY(am)` 释放引擎自身
- **错误返回**：`int`（0 成功 / -1 失败）或 `ItemPtr`（`INVALID_ITEM_PTR` = 失败）
- **不得过度防御**：只在系统边界（外部输入）做校验，内部不变量用 `assert()`
- - **可以修改src下代码的测试用例写到tests, tmp/src下的代码的测试用例写到tmp/tests，src下代码的测试用例写到tests 主要通过tmp/tests移植** 
- **数据库的知识和实现必须采用 PG 风格对齐**：函数参数、核心命名和代码组织优先参考
  `/home/unvdb/cproject/UDB-TX`，同时只实现嵌入式环境真正需要的子集，不追求 PostgreSQL
  1:1 完整复制，也不引入没有明确需求的新架构。

## 禁止行为
- **不得修改 `src/` 下代码或头文件；`src` 是用户手写产品化核心，AI 默认只允许修改 `tmp/` 和文档**
- **不得引入顺序 iid 计数器**（如 `next_internal_id`、新增 `emb_iid` 字段）
- **不得重新引入已删除的** `emb_iid` / `"_emb_iid"` **兼容字段**；当前内部列名是
  `"_vector_id"`，算法库使用中立的 `external_ref`
- **不得在 `src/store.h` 中定义 `EmbeddingStore` 结构体主体** — 与 `tmp/src/embedding_store.h` 冲突
- **不得对 `get(seq_idx)` 统一改为 ctid** — 三引擎地址空间不同，无法统一
- **修改代码后必须运行 `make -C tmp test` 验证全部测试通过**（测试数量随新增测试文件增长）

## 测试规范

```c
// 所有测试文件遵循此模式
static int pass_count = 0, fail_count = 0;
#define CHECK(cond, msg) do { \
    if (cond) { pass_count++; printf("[PASS] %s\n", msg); } \
    else      { fail_count++; printf("[FAIL] %s\n", msg); } \
} while(0)
// main() 返回 fail_count > 0 ? 1 : 0
```

Catalog/Index 测试套路：
```c
mustdb_table_create() → mustdb_relation_create_*_index()
    → mustdb_sys_index → reopen → relation_find_index()
```

## 关键 API 速查

```c
// RowStore（heap，PG heapam 风格）
ItemPtr heap_insert(RowStore*, Transaction*, u32 cid,
                    const Datum* values, u64 null_bits);
int     heap_fetch(RowStore*, ItemPtr tid, const RowSnapshot*, TxnId own_xid,
                   TupleHdr* out_hdr, Datum* out_vals);
int     heap_update(RowStore*, ItemPtr old_tid, Transaction*, u32 cid,
                    const Datum* values, u64 null_bits, ItemPtr* out_new_tid);
bool    heap_delete(RowStore*, ItemPtr tid, Transaction*, u32 cid);
RsWriteResult heap_update_with_options(RowStore*, ItemPtr old_tid, Transaction*, u32 cid,
                                       const Datum* values,
                                       u64 null_bits, const RsWriteOptions* opts,
                                       ItemPtr* out_new_tid);
RsWriteResult heap_delete_with_options(RowStore*, ItemPtr tid, Transaction*, u32 cid,
                                       const RsWriteOptions* opts);
int     txnmgr_vacuum_freeze_heap(TransactionManager*, RowStore*,
                                  RowFreezeMaintenanceStats* out);
RowHintStats row_store_hint_stats(RowStore*);
u64     heap_relation_slot_count(RowStore*);
u64     heap_ctid_to_seqidx(RowStore*, ItemPtr tid);

// EmbeddingStore
ItemPtr    embedding_store_append(EmbeddingStore*, const f32* vector, usize dim);
const f32* embedding_store_get_ptr(const EmbeddingStore*, ItemPtr emb_ctid, usize dim);
int        embedding_store_read(const EmbeddingStore*, ItemPtr emb_ctid, usize dim, f32* out);
int        embedding_store_write_at(EmbeddingStore*, ItemPtr emb_ctid, const f32* vector, usize dim);
int        embedding_store_free(EmbeddingStore*, ItemPtr emb_ctid);
usize      embedding_store_count(const EmbeddingStore*);

// StorageTable（显式事务 CRUD）
int   storage_table_insert_chunk(StorageTable*, Transaction*, u32 cid,
                                 const MustDbChunk*, ItemPtr* out_heap_ctids);
RsWriteResult storage_table_update_row(StorageTable*, Transaction*, u32 cid,
                                       ItemPtr old_heap_ctid, const MustDbChunk*,
                                       const RsWriteOptions*, ItemPtr* out_new_heap_ctid);
RsWriteResult storage_table_delete_row(StorageTable*, Transaction*, u32 cid,
                                       ItemPtr heap_ctid, const RsWriteOptions*);
int   storage_table_get_row_by_ctid(StorageTable*, ItemPtr heap_ctid, Datum* out_vals, u64* out_null_bits);
usize storage_table_seq_scan(StorageTable*, const VectorCondition*, TableFilter, void*,
                             const MvccSnapshot*, TxnId own_xid, TableSearchResult*, usize max_results);

// Collection 写冲突策略
int collection_set_write_options(Collection*, const CollectionWriteOptions*);
CollectionWriteOptions collection_get_write_options(const Collection*);

// ViIndex LSM
int   vi_index_insert(ViIndex*, u64 vector_id, const f32* vector, usize dim);
usize vi_index_search(ViIndex*, const f32* query, usize dim, usize k, VaTopKEntry* out, usize out_cap);
int   vi_index_delete(ViIndex*, u64 vector_id);
int   vi_index_flush_mutable(ViIndex*);

// vtable 宏
TAM_APPEND(am, data)                                 // 单行追加
TAM_GET(am, seq_idx, out)                            // 按 seq_idx 读取
TAM_COUNT(am)
TAM_DESTROY(am)
TAM_APPEND_CHUNK(am, chunk, ctx)                     // 批量插入（主路径）
TAM_READ_CHUNK(am, ctx, out_chunk, out_idx, count)   // 批量读取（对称主路径）
TAM_SCAN(am, ctx, out_chunk)                         // 顺序扫描
TAM_GET_HDR(am, ctid, out)                           // 读 TupleHdr
TAM_PREPARE_READ(am, ctid, ctx)                      // 零拷贝预读（pin + shared lock）
TAM_FREE_READ(am, ctx)                               // 释放预读资源
TAM_UPDATE(am, v, payloads, null_bits, ctx)          // 更新
TAM_DELETE(am, ctx)                                  // 删除
```

## StorageTable CRUD 必须通过 TAM vtable（架构约束）

`storage_table_*` 层（增删改查）**禁止**直接调用底层 Store API（`row_store_*`、
`embedding_store_*`），也**禁止**将 `TableAm*` 强制转换为具体引擎类型
（`TamHeapTable*`、`TamColTable*`）来访问成员。

必须且只能使用以下 TAM vtable 宏：

```c
TAM_SCAN(am, ctx, chunk)                  // 顺序扫描（heap 驱动）
TAM_GET_HDR(am, ctid, out_hdr)            // 读取 TupleHdr（仅 heap engine 有效）
TAM_PREPARE_READ(am, ctid, ctx)           // 预加载 heap tuple 到 TamReadCtx（pin + shared lock）
TAM_FREE_READ(am, ctx)                    // 释放 prepare_read 分配的资源
TAM_UPDATE(am, v, payloads, null_bits, ctx) // 更新（soft-delete old + append new）
TAM_DELETE(am, ctx)                       // 删除（支持 ctx->xid 事务路径）
TAM_APPEND_CHUNK(am, chunk, ctx)          // 批量插入
TAM_READ_CHUNK(am, ctx, out_chunk, ...)   // 批量读取
```

**各引擎** vtable 内部实现可以（也必须）直接访问自己的 store：heap 路径访问
`TamHeapTable->store`（RowStore），col 路径访问 `TamColTable->cs`（ColumnStore）。
封装边界在 `StorageTable` 层，不在引擎层。


# 参考代码
/home/unvdb/cproject/UDB-TX 是postgresql 源码
/home/unvdb/cproject/sqlite 是sqlite 源码
/home/unvdb/cproject/duckdb-1.0.0 是duckdb 源码
/home/unvdb/cproject/zvec 是zvec源码
/home/unvdb/rsproject/qdrant-1.18.2 是qdrant源码

# 记忆系统设计
Corvus 应做成一个本地、可嵌入、可审计的 Agent Memory Database，而不是“把聊天记录丢进向量库”。

  参考策略

  - OpenClaw：参考记忆生命周期、compaction 前 flush、后台 consolidation、provenance/taint、recall-loop 防护。
  - Mem0：参考 add/search/update/delete/history、user/agent/run scope，以及 ADD/UPDATE/DELETE/NOOP 写入决策。
  - Graphiti：后续参考 episode、时态事实、来源链、valid_from/valid_to，不要引入 Neo4j。
  - Letta：参考有限 token context projection，不照搬完整 Agent runtime。

  目标架构

  Codex / Claude / DeepSeek Harness
                |
          MCP + lifecycle adapter
                |
            Corvus (Rust)
                |
       +--------+---------+
       |                  |
   MustDB              libostore
   找什么               内容在哪里
   metadata             原始对话 / 文件 / artifact
   fulltext             immutable object
   vector               chunk range
   version              checksum / GC
   provenance

  四层数据模型

  1. Episode
     原始对话、tool output、PDF、代码、artifact
     libostore 是权威内容；MustDB 只存 object_id、范围、来源和时间。

  2. Recall Candidate
     从 episode 切出的 chunk、摘要、embedding、BM25、标签、scope。
     可被检索，但不是永久事实。

  3. Durable Memory
     已确认的 fact / decision / preference / procedure。
     有 source、trust、version、expiry、supersede、有效时间。

  4. Context Projection
     根据当前任务，从 candidate 和 current durable memory 中召回少量内容，
     控制 token budget，注入 Agent context。

  MustDB 的建议 collection

   Collection          内容                                                    索引
  ━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   episodes            object 引用、session、时间、来源、scope                 object/source/time
  ──────────────────  ──────────────────────────────────────────────────────  ─────────────────────────────────────
   memory_chunks       chunk 摘要、object_id + byte_range、embedding           fulltext + vector
  ──────────────────  ──────────────────────────────────────────────────────  ─────────────────────────────────────
   durable_memories    当前与历史事实、决策、偏好、procedure                   fulltext + vector + type/time/scope
  ──────────────────  ──────────────────────────────────────────────────────  ─────────────────────────────────────
   memory_links        candidate 与 durable memory 的 source/supersede 关系    后续可接 GraphAccess

  大原文永远不写入 MustDB heap 主路径：

  libostore:
    episodes/<content_hash>
    artifacts/<content_hash>

  MustDB:
    memory_id -> object_id + byte_range

  关键写入流程

  1. Harness 交付 session / tool result / document
  2. Corvus 先把原文 publish 到 libostore
  3. Corvus 在 MustDB 写 Episode 元数据
  4. 切 chunk，生成 embedding，写 fulltext/vector candidate
  5. 后台任务筛选 candidate
  6. 模型只返回结构化提议：
     ADD / SUPERSEDE / EXPIRE / NOOP
  7. Corvus 用确定性规则验证 scope、来源、trust、冲突与版本
  8. MustDB 事务提交 DurableMemory

  模型不能直接把一句话写成最终事实。它只能提出候选；Corvus 必须验证后才能晋升。

  关键读取流程

  任务开始 / 用户发问
    -> Corvus 按 user + agent + workspace + repo + permission 过滤
    -> MustDB BM25 + vector + time/type/trust 混合召回
    -> 优先当前有效 DurableMemory
    -> 需要证据时读取 Episode 的 libostore range
    -> 生成受 token budget 限制的 ContextProjection
    -> 注入 Codex / Claude / DeepSeek

  必须从第一版就有的安全规则

  - 每条 DurableMemory 至少有一个 EpisodeRef。
  - 来源分类：owner / agent / untrusted / system。
  - untrusted tool/web/document 内容可以保存和检索，但不能自动晋升或自动注入为指令。
  - recall 返回的内容不可再次作为新事实自动抽取，防止 recall loop。
  - 事实变化走 supersede，不能直接覆盖且丢失历史。
  - 删除先删 MustDB 引用和索引，object 在无引用、无 reader pin 后由 libostore GC。

  实现顺序

  1. 已完成：Rust workspace、MustDB/libostore FFI 链接。
  2. corvus-core：Episode、MemoryCandidate、DurableMemory、scope/provenance 类型。
  3. remember：原文 publish -> MustDB metadata/chunk/index 的可靠写入。
  4. recall/read/forget：hybrid 检索、证据回读、延迟 GC。
  5. corvus-mcp：corvus_recall、corvus_remember、corvus_read、corvus_forget。
  6. harness adapter：session start recall、turn/session end ingest、compaction flush。
  7. 后台 consolidation：去重、冲突、过期、ADD/SUPERSEDE/EXPIRE/NOOP。
  8. 时态事实：observed_at / valid_from / valid_to。
  9. 真有关系推理需求后，再利用 MustDB GraphAccess，不要提前把系统改成图数据库。

  这条路线比 OpenClaw 的文件记忆更可靠，也避免一开始把 Corvus 做成 Graphiti、MemOS 那样过重的通用 Agent 平台。
  
  参考代码
/home/unvdb/aiproject/graphiti-main  
/home/unvdb/aiproject/mem0-main
/home/unvdb/aiproject/MemOS-main  
/home/unvdb/aiproject/openclaw-main/extensions/memory-core
/home/unvdb/aiproject/MemMachine-main
