#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "store.h"
#include "table.h"
#include "segment.h"
#include "must.h"
#include "vector.h"

/* ============================================================
 * EmbeddingStore：向量页式存储
 *
 * 这里的 EmbeddingStore 是向量数据的权威存储。每个向量按固定长度
 * dimension * sizeof(f32) 连续写入 BlockSegment 页面，外部通过 ItemPtr
 * 形式的 emb_ctid 定位到具体页面和页内 slot。
 * ============================================================ */

/* 初始化向量存储：计算单个向量字节数、每页可容纳向量数，并创建第一页。 */
void EmbeddingStore_init(EmbeddingStore* store, i16 dimension)
{
    /* 维度和派生尺寸在生命周期内保持不变，后续定位都依赖这些值。 */
    store->dimension = dimension;
    store->count = 0;
    store->elem_size = (usize)dimension * sizeof(f32);
    store->vecs_per_blk = BLOCK_SIZE / store->elem_size;

    /* SegmentTree 管理多个 BlockSegment；第一页从逻辑 start=0 开始。 */
    SegmentTree_init(&store->tree);
    BlockSegment* first = BlockSegment_create2(0);
    /* 内存模式下使用 INVALID_BLOCK，表示尚未绑定真实磁盘 block_id。 */
    first->block = Block_create(INVALID_BLOCK);
    segmentTree_append_segment(&store->tree, (SegmentBase*)first);

    /* free_list 保存可复用的 emb_ctid；last_ctid 记录最近一次写入位置。 */
    Vector_init(&store->free_list, sizeof(ItemPtr), 0);
    store->last_ctid = INVALID_ITEM_PTR;
    LWLockInit(&store->lock, "EmbeddingStore.lock");
}

/* 释放所有向量页面和 SegmentTree 元数据。 */
void EmbeddingStore_deinit(EmbeddingStore* store)
{
    if (!store) return;
    SegmentTree_deinit(&store->tree, BlockSegment_destroy);
    store->count = 0;
}

/* 追加一个向量到当前高水位线；当前页空间不足时创建新页。 */
static void embedding_store_append(EmbeddingStore* store, MustDbVector* vec)
{
    BlockSegment* seg = (BlockSegment*)segmentTree_get_last_segment(&store->tree);
    usize start_pos = seg->byte_offset;
    usize remaining_space = BLOCK_SIZE - start_pos;

    if (remaining_space >= store->elem_size)
    {
        /* 当前页还能容纳完整向量，直接拷贝到 byte_offset 指向的位置。 */
        data_ptr_t data = segment_get_data(seg) + start_pos;
        memcpy(data, vec->data, store->elem_size);
        seg->base.count++;
        seg->byte_offset += store->elem_size;
    }
    else
    {
        /* 当前页放不下完整向量，追加一个新的 BlockSegment 页面。 */
        BlockSegment* new_seg = BlockSegment_create2(seg->base.start + seg->base.count);
        new_seg->block = Block_create(INVALID_BLOCK);
        segmentTree_append_segment(&store->tree, (SegmentBase*)new_seg);

        /* 新页从 offset=0 写入第一个向量。 */
        data_ptr_t data = segment_get_data(new_seg);
        memcpy(data, vec->data, store->elem_size);
        new_seg->base.count++;
        new_seg->byte_offset = store->elem_size;
    }
    store->count++;
}

/* 将顺序 row_idx 转成 emb_ctid：block=页序号，slot=页内向量序号。 */
static inline ItemPtr emb_ctid_from_row_idx(const EmbeddingStore* store, usize row_idx)
{
    usize seg_idx = row_idx / store->vecs_per_blk;
    usize local_idx = row_idx % store->vecs_per_blk;
    return make_item_ptr((block_id_t)seg_idx, (u16)local_idx);
}

/* 按 emb_ctid 原地覆盖向量；用于 free_list 槽位复用。 */
static void embedding_store_write_at_ctid(EmbeddingStore* store, ItemPtr ctid, MustDbVector* vec)
{
    usize seg_idx = (usize)item_ptr_block_id(ctid);
    usize local_idx = (usize)item_ptr_slot(ctid);
    SegmentNode* sn = (SegmentNode*)vector_get(store->tree.nodes, seg_idx);
    if (!sn) return;
    BlockSegment* seg = (BlockSegment*)sn->node;
    /* 页内偏移 = slot * elem_size，向量本身固定长度，因此无需 slot directory。 */
    u8* dst = (u8*)segment_get_data(seg) + local_idx * store->elem_size;
    memcpy(dst, vec->data, store->elem_size);
}

/* 插入向量并返回实际 emb_ctid；优先复用 free_list，否则顺序 append。 */
ItemPtr embeddingStore_append_and_get_ctid(EmbeddingStore* store, MustDbVector* vec)
{
    usize free_n = vector_size(&store->free_list);
    if (free_n > 0)
    {
        /* 有可复用槽位：弹出一个旧 emb_ctid 并在原位置覆盖新向量。 */
        ItemPtr ctid;
        vector_pop_back(&store->free_list, &ctid);
        embedding_store_write_at_ctid(store, ctid, vec);
        store->last_ctid = ctid;
        return ctid;
    }
    else
    {
        /* 无空闲槽位：在高水位线末尾追加，count 增长。 */
        usize row_idx = store->count;
        embedding_store_append(store, vec);
        store->last_ctid = emb_ctid_from_row_idx(store, row_idx);
        return store->last_ctid;
    }
}

/* 按 emb_ctid 返回向量的零拷贝只读指针；调用方不能释放该指针。 */
const f32* embedding_store_get_ptr_ctid(EmbeddingStore* store, ItemPtr emb_ctid)
{
    LWLockAcquire(&store->lock, LW_SHARED);
    /* emb_ctid 的 block 部分对应 SegmentTree 下标，slot 部分对应页内向量序号。 */
    usize seg_idx = (usize)item_ptr_block_id(emb_ctid);
    usize local_idx = (usize)item_ptr_slot(emb_ctid);
    SegmentNode* sn = (SegmentNode*)vector_get(store->tree.nodes, seg_idx);
    const f32* ptr = NULL;
    if (sn)
    {
        BlockSegment* seg = (BlockSegment*)sn->node;
        /* 直接返回页内地址，避免把向量复制到临时缓冲区。 */
        ptr = (const f32*)((u8*)segment_get_data(seg) + local_idx * store->elem_size);
    }
    LWLockRelease(&store->lock);
    return ptr;
}

/* ============================================================
 * HeapStore：空实现占位
 *
 * 产品化 HeapStore 将由人工重新参考 tmp/src/row_store 实现。这里保留
 * store.h 中已有 API 符号，避免依赖方链接失败；所有 DML/读取接口均返回
 * 空结果或失败状态。
 * ============================================================ */

void HeapStore_init(HeapStore* store, const TableSchema* schema, BlockManager* bm)
{
    if (!store) return;
    memset(store, 0, sizeof(*store));
    store->schema = schema;
    store->block_manager = bm;
    store->hint_free_seg = NULL;
    store->next_txn_id = 0;
    store->page_count = 0;
    SegmentTree_init(&store->tree);
    LWLockInit(&store->lock, "HeapStore.content_lock");
}

void HeapStore_deinit(HeapStore* store)
{
    if (!store) return;
    SegmentTree_deinit(&store->tree, BlockSegment_destroy);
    store->schema = NULL;
    store->block_manager = NULL;
    store->hint_free_seg = NULL;
    store->next_txn_id = 0;
    store->page_count = 0;
}

int heapStore_deform_tuple(const HeapTupleRef* ref, Datum* out, usize ncols)
{
    (void)ref;
    (void)out;
    (void)ncols;
    return -1;
}

u64 heapStore_slot_count(HeapStore* store)
{
    (void)store;
    return 0;
}

ItemPtr heapStore_insert(HeapStore* store, TxnId xid, ItemPtr emb_ctid, const Datum* values,
                         u64 null_bits)
{
    (void)store;
    (void)xid;
    (void)emb_ctid;
    (void)values;
    (void)null_bits;
    return INVALID_ITEM_PTR;
}

int heapStore_get_by_ctid(HeapStore* store, const TableSchema* schema, ItemPtr ctid, HeapTuple* out)
{
    (void)store;
    (void)schema;
    (void)ctid;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

TxnId heapStore_delete_by_ctid(HeapStore* store, ItemPtr ctid)
{
    (void)store;
    (void)ctid;
    return INVALID_TXN_ID;
}

TxnId heapStore_update_by_ctid(HeapStore* store, ItemPtr old_ctid, HeapTuple* new_tuple)
{
    (void)store;
    (void)old_ctid;
    (void)new_tuple;
    return INVALID_TXN_ID;
}

void heapStoreIter_begin(HeapStoreIter* iter, HeapStore* store)
{
    if (!iter) return;
    memset(iter, 0, sizeof(*iter));
    iter->store = store;
}

const TupleHdr* heapStoreIter_next(HeapStoreIter* iter)
{
    (void)iter;
    return NULL;
}

void heapStoreIter_end(HeapStoreIter* iter)
{
    if (!iter) return;
    memset(iter, 0, sizeof(*iter));
}
