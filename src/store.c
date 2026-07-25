#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "store.h"
#include "table.h"
#include "segment.h"
#include "vb_type.h"
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
static void embedding_store_append(EmbeddingStore* store, VectorBase* vec)
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
static void embedding_store_write_at_ctid(EmbeddingStore* store, ItemPtr ctid, VectorBase* vec)
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
ItemPtr embeddingStore_append_and_get_ctid(EmbeddingStore* store, VectorBase* vec)
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
 * HeapStore：行存储页面
 *
 * HeapStore 使用类似 PostgreSQL slotted page 的布局：
 *   [page header][slot array ->][free space][<- tuple data]
 * pd_lower 指向 slot 数组尾部，pd_upper 指向 tuple 数据区起点。
 * ============================================================ */

/* 读取页面头部 pd_lower：slot 数组的结束位置。 */
static inline u16* heapStore_pd_lower(u8* page)
{
    return (u16*)page;
}

/* 读取页面头部 pd_upper：tuple 数据区的起始位置。 */
static inline u16* heapStore_pd_upper(u8* page)
{
    return (u16*)(page + 2);
}

/* 读取页面标志位；当前主要用于 PD_HAS_FREE_LINES 空闲 slot 提示。 */
static inline u16* heapStore_pd_flags(u8* page)
{
    return (u16*)(page + 4);
}

/* slot 数组紧跟在固定页面头后面，每个 slot 描述一个 tuple 的 offset/len。 */
static inline TupleSlotId* heapStore_slots(u8* page)
{
    return (TupleSlotId*)(page + HS_BLOCK_HDR_SIZE);
}

/* 页面可用空间 = tuple 数据区起点 - slot 数组尾部。 */
static inline u16 heapStore_free_space(u8* page)
{
    return *heapStore_pd_upper(page) - *heapStore_pd_lower(page);
}

/*
 * datum_deform_copy — read one column from page into Datum (copy path).
 *
 * 中文说明：从序列化 tuple 的列数据区反序列化一列到 Datum。
 * 固定长度类型直接按值写入 Datum；变长类型需要分配内存并复制。
 *
 * Fixed-size types: by-value Datum, no malloc.
 * Varlena types (TEXT/BYTEA/JSONB): malloc'd copy — caller must free.
 *
 * Returns pointer past the column bytes on success, NULL on malloc failure.
 */
static const u8* datum_deform_copy(const u8* p, TupleColType type, Datum* out)
{
    switch (type)
    {
        case TUPLE_COL_BOOL:
            /* bool 在页内用 1 字节保存。 */
            *out = BoolGetDatum(*p != 0);
            p += 1;
            break;
        case TUPLE_COL_I32:
        {
            /* 使用 memcpy 避免未对齐访问问题。 */
            i32 v;
            memcpy(&v, p, 4);
            *out = Int32GetDatum(v);
            p += 4;
            break;
        }
        case TUPLE_COL_I64:
        {
            i64 v;
            memcpy(&v, p, 8);
            *out = Int64GetDatum(v);
            p += 8;
            break;
        }
        case TUPLE_COL_F32:
        {
            f32 v;
            memcpy(&v, p, 4);
            *out = Float32GetDatum(v);
            p += 4;
            break;
        }
        case TUPLE_COL_F64:
        {
            f64 v;
            memcpy(&v, p, 8);
            *out = Float64GetDatum(v);
            p += 8;
            break;
        }
        default:
            /* 未识别类型按 NULL/空 Datum 处理，保持调用方可继续遍历。 */
            *out = (Datum)0;
            break;
    }
    return p;
}

/*
 * rs_deform_all — deserialize all non-null columns from page into Datum[].
 *
 * 中文说明：根据 schema 和 null bitmap，顺序解析 tuple 中所有非 NULL 列。
 * NULL 列在 tuple 数据区没有物理字节，直接输出 Datum=0。
 *
 * Returns 0 on success, -1 on malloc failure (partial datums freed on error).
 */
static int heapStore_deform_all(const u8* col_data, const TableSchema* schema, u64 null_bits,
                                Datum* out)
{
    const u8* p = col_data;
    for (u16 i = 0; i < schema->ncols; i++)
    {
        if (null_bits & ((u64)1 << i))
        {
            /* null_bits 中对应 bit 为 1 表示该列为 NULL，页内不占数据。 */
            out[i] = (Datum)0;
            continue;
        }
        p = datum_deform_copy(p, schema->cols[i], &out[i]);
        if (!p)
        {
            /* 分配失败时释放前面已经解析出来的变长 Datum，避免泄漏。 */
            u64 done = 0;
            for (u16 j = 0; j < i; j++)
            {
                if (null_bits & ((u64)1 << j)) continue;
                TupleColType ct = schema->cols[j];
                if (ct == TUPLE_COL_TEXT || ct == TUPLE_COL_JSONB)
                {
                    void* ptr = DatumGetPointer(out[j]);
                    if (ptr) free(ptr);
                }
                done |= ((u64)1 << j);
            }
            (void)done;
            return -1;
        }
    }
    return 0;
}

/* 初始化空 HeapStore 页面：slot 从头部后增长，tuple 从页尾向前增长。 */
static void heapStore_page_init(u8* page)
{
    *heapStore_pd_lower(page) = (u16)HS_BLOCK_HDR_SIZE;
    *heapStore_pd_upper(page) = (u16)BLOCK_SIZE;
    *heapStore_pd_flags(page) = 0;
}

/* 初始化行存储：绑定 schema / BlockManager，并创建第一页。 */
void HeapStore_init(HeapStore* store, const TableSchema* schema, BlockManager* bm)
{
    store->block_manager = bm;
    store->schema = schema;
    store->hint_free_seg = NULL;
    LWLockInit(&store->lock, "HeapStore.content_lock");

    SegmentTree_init(&store->tree);

    /* 第一页既可能来自真实 BlockManager，也可能是 standalone 本地 block_id=0。 */
    BlockSegment* seg = BlockSegment_create2(0);
    block_id_t bid;
    if (bm != NULL)
    {
        /* 磁盘模式：向 BlockManager 申请真实物理 block_id。 */
        bid = VCALL(bm, get_free_block_id);
        seg->block_manager = bm;
    }
    else
    {
        /* 内存/standalone 模式：使用本地顺序 block_id。 */
        bid = 0;
    }
    seg->block_id = bid;
    seg->block = Block_create(bid);
    /* 新页必须先初始化 slotted-page header。 */
    heapStore_page_init((u8*)segment_get_data(seg));
    segmentTree_append_segment(&store->tree, (SegmentBase*)seg);
}

/* 释放 HeapStore 的所有页面段。 */
void HeapStore_deinit(HeapStore* store)
{
    if (!store) return;
    SegmentTree_deinit(&store->tree, BlockSegment_destroy);
    store->page_count = 0;
}

/*
 * heap_deform_tuple — populate Datum[] from a HeapTupleRef.
 *
 * 中文说明：对外的 tuple 反序列化入口，把 HeapTupleRef 指向的页内列数据解析成 Datum[]。
 *
 * Fixed-size types: by-value Datum (inline, no malloc).
 * Varlena types:    malloc'd copy — caller owns; free with datum_array_free.
 * Null columns:     bit set in ref->hdr->null_bits — Datum = 0 (unused).
 *
 * Returns 0 on success, -1 on malloc failure.
 */
int heapStore_deform_tuple(const HeapTupleRef* ref, Datum* out, usize ncols)
{
    if (!ref->col_data || !out) return -1;
    /* use_ncols 当前仅保留接口语义；实际解析仍按 ref->schema 全列执行。 */
    usize use_ncols = ncols < (usize)ref->schema->ncols ? ncols : (usize)ref->schema->ncols;
    return heapStore_deform_all(ref->col_data, ref->schema, ref->hdr->null_bits, out);
    (void)use_ncols;
}

/* 返回物理 slot 高水位线：最后一个 segment 的 start + count。 */
u64 heapStore_slot_count(HeapStore* store)
{
    SegmentBase* last = segmentTree_get_last_segment(&store->tree);
    if (!last) return 0;
    return (u64)(last->start + last->count);
}

/* 按列类型把一个 Datum 序列化到页面缓冲区，返回写入后的指针。 */
static u8* wirte_col(u8* p, TupleColType type, Datum d)
{
    switch (type)
    {
        case TUPLE_COL_BOOL:
            /* bool 固定 1 字节。 */
            *p = DatumGetBool(d) ? 1u : 0u;
            return p + 1;
        case TUPLE_COL_I32:
            /* 固定长度数值类型直接以机器字节序写入当前格式。 */
            i32 v1 = DatumGetInt32(d);
            memcpy(p, &v1, sizeof(i32));
            return p += sizeof(i32);
        case TUPLE_COL_I64:
            i64 v2 = DatumGetInt64(d);
            memcpy(p, &v2, sizeof(i64));
            return p + sizeof(i64);
        case TUPLE_COL_F32:
            f32 v3 = DatumGetFloat32(d);
            memcpy(p, &v3, sizeof(f32));
            return p + sizeof(f32);
        case TUPLE_COL_F64:
            f64 v4 = DatumGetFloat64(d);
            memcpy(p, &v4, sizeof(f64));
            return p + sizeof(f64);
        case TUPLE_COL_TEXT:
        {
            /* 变长文本格式：[u32 content_len][data...]，内存格式与页内格式一致。 */
            u8* ptr = (u8*)DatumGetPointer(d);
            u32 len = 0;
            memcpy(&len, ptr, 4);
            memcpy(p, ptr, (usize)4 + len);
            p += (usize)4 + len;
            return p;
        }
        default:
            /* 正常不会到这里；保持指针不变让上层断言/校验发现问题。 */
            return p; /* should not happen */
    }
}

/* ============================================================
 * Tuple 序列化辅助函数
 *
 * 这些函数负责计算 tuple 字节大小、把 TupleHdr 和列值连续写入页内。
 * 序列化路径不分配中间 tuple buffer，最终直接写入 slotted page。
 * ============================================================ */

/* 计算单列序列化后的字节数。 */
static usize compute_col_size(TupleColType type, Datum d)
{
    switch (type)
    {
        case TUPLE_COL_BOOL:
            return 1;
        case TUPLE_COL_I32:
            return 4;
        case TUPLE_COL_I64:
            return 8;
        case TUPLE_COL_F32:
            return 4;
        case TUPLE_COL_F64:
            return 8;
        case TUPLE_COL_TEXT:
        {
            /* TEXT 的 Datum 指向 [u32 len][data...]，页内直接沿用同一格式。 */
            u32 len = 0;
            memcpy(&len, DatumGetPointer(d), 4);
            return (usize)4 + len;
        }
        default:
            return 0;
    }
}

/* 计算完整 tuple 大小：固定 TupleHdr + 所有非 NULL 列的序列化大小。 */
static usize compute_tuple_size(const TableSchema* schema, const Datum* vals)
{
    usize size = sizeof(TupleHdr);
    for (u16 i = 0; i < schema->ncols; i++)
    {
        /* vals==NULL 时只计算 header，保留当前调用路径的空值语义。 */
        // if (null_bits & ((u64)1 << i)) continue;
        if (!vals) continue;
        size += compute_col_size(schema->cols[i], vals[i]);
    }
    return size;
}

/* 将 tuple 直接序列化到目标内存；dest 必须已预留 compute_tuple_size 的空间。 */
static void serialize_tuple_into(u8* dst, const TupleHdr* hdr, const TableSchema* schema,
                                 const Datum* vals)
{
    /* TupleHdr 固定放在 tuple 起始位置，后面紧跟列数据。 */
    memcpy(dst, hdr, sizeof(TupleHdr));
    u8* p = dst + sizeof(TupleHdr);
    for (u16 i = 0; i < schema->ncols; i++)
    {
        /* NULL 列不写物理数据，只由 hdr->null_bits 表示。 */
        if (hdr->null_bits & ((u64)1 << i)) continue;
        if (!vals) continue;
        p = wirte_col(p, schema->cols[i], vals[i]);
    }
}

/* ============================================================
 * rs_page_compact — in-place page defragmentation (PG PageRepairFragmentation)
 *
 * 中文说明：页内碎片整理。vacuum 后 LP_UNUSED slot 会留下空洞，
 * compact 将仍然存活的 tuple 重新挪到页尾连续区域，并更新 slot->lp_off。
 *
 * Moves all live tuples to the top of the page (high byte addresses),
 * closing gaps left by LP_UNUSED (vacuumed) slots.
 * Updates each live slot's lp_off; resets pd_upper.
 * Dead slots (lp_len == 0) are left unchanged in the slot array.
 *
 * Move direction: always upward (src ≤ dest), so memmove is correct even
 * when adjacent blocks overlap.  O(page_size).
 * ============================================================ */
static void heapStore_page_compact(u8* page)
{
    u16 pd_lower = *heapStore_pd_lower(page);
    /* slot 数量由 pd_lower 推导，LP_UNUSED slot 仍保留在 slot array 中。 */
    usize n_slots = (pd_lower - HS_BLOCK_HDR_SIZE) / HS_SLOT_SIZE;
    u16 new_upper = (u16)BLOCK_SIZE;

    for (usize i = 0; i < n_slots; i++)
    {
        TupleSlotId* sl = &heapStore_slots(page)[i];
        if (sl->lp_len == 0) continue; /* LP_UNUSED: skip */

        /* 从页尾向前重新铺放 live tuple，保持 tuple 数据连续。 */
        u16 tup_len = sl->lp_len;
        new_upper -= tup_len;
        if (sl->lp_off != new_upper)
        {
            /* memmove 支持重叠区域，适合页内原地整理。 */
            memmove(page + new_upper, page + sl->lp_off, tup_len);
            sl->lp_off = new_upper;
        }
    }

    /* pd_upper 指向整理后 live tuple 区域的起点。 */
    *heapStore_pd_upper(page) = new_upper;
}

/* 根据物理 block_id 查找 Segment；定义在后面，前置声明供复用路径使用。 */
static BlockSegment* heapStore_find_seg_by_block(const HeapStore* store, block_id_t block_id);

/* 简单递增事务号分配器；HeapStore 内部删除/插入路径使用。 */
static TxnId rs_alloc_xid(HeapStore* store)
{
    return ++store->next_txn_id;
}

/* 在 vacuum 释放出来的 LP_UNUSED slot 上原地复用并写入新 tuple。 */
static ItemPtr heapStore_reuse_slot(HeapStore* store, ItemPtr ctid, TupleHdr* hdr,
                                    const Datum* vals)
{
    /* ctid 定位到具体页面和 slot；只有 lp_len==0 的 slot 才允许复用。 */
    BlockSegment* seg = heapStore_find_seg_by_block(store, item_ptr_block_id(ctid));
    if (!seg) return INVALID_ITEM_PTR;

    u16 slot_idx = item_ptr_slot(ctid);
    u8* page = (u8*)segment_get_data(seg);
    if ((usize)slot_idx >= seg->base.count) return INVALID_ITEM_PTR;

    TupleSlotId* sl = &heapStore_slots(page)[slot_idx];
    if (sl->lp_len != 0) return INVALID_ITEM_PTR; /* not LP_UNUSED: safety check */

    usize ser_size = compute_tuple_size(store->schema, vals);
    assert(ser_size <= HS_MAX_TUPLE_SIZE);

    /* 复用已有 slot，不增加 pd_lower；只需要从 tuple 数据区重新分配 ser_size。 */
    if ((usize)heapStore_free_space(page) < ser_size)
    {
        /* 空闲空间不足时先整理页面，把 LP_UNUSED 留下的碎片收回来。 */
        heapStore_page_compact(page);
        if ((usize)heapStore_free_space(page) < ser_size)
            return INVALID_ITEM_PTR; /* page is genuinely full even after compaction */
    }

    /* 写入物理自指 ctid：(block_id, slot_idx)。 */
    hdr->t_ctid = make_item_ptr(seg->block_id, slot_idx);

    /* tuple 数据从页尾向前分配。 */
    *heapStore_pd_upper(page) -= (u16)ser_size;
    u16 tup_off = *heapStore_pd_upper(page);

    /* 直接序列化到页内，避免中间缓冲区。 */
    serialize_tuple_into(page + tup_off, hdr, store->schema, vals);

    /* 重新激活该 slot。 */
    sl->lp_off = tup_off;
    sl->lp_len = (u16)ser_size;

    return ctid;
}

/* 在当前页分配一个新 slot 和 tuple 空间，返回可直接写入的页内地址。 */
static u8* heapStore_page_reserve_slot(u8* page, u16 len, u16* out_slot_idx)
{
    /* tuple 从 pd_upper 向前增长，slot 从 pd_lower 向后增长。 */
    u16 pd_upper = *heapStore_pd_upper(page);
    u16 tup_off = (u16)(pd_upper - len);

    /* 当前 slot_idx 等于已有 slot 数量。 */
    u16 slot_idx = (u16)((*heapStore_pd_lower(page) - HS_BLOCK_HDR_SIZE) / HS_SLOT_SIZE);
    TupleSlotId* slot = heapStore_slots(page) + slot_idx;
    slot->lp_off = tup_off;
    slot->lp_len = len;

    /* 同时推进 slot 数组尾部和 tuple 数据区起点。 */
    *heapStore_pd_lower(page) = (u16)(*heapStore_pd_lower(page) + HS_SLOT_SIZE);
    *heapStore_pd_upper(page) = tup_off;

    if (out_slot_idx) *out_slot_idx = slot_idx;
    return page + tup_off;
}

/* 追加 tuple 到高水位线页面；空间不足时创建新页面。 */
static ItemPtr heapStore_append_tuple(HeapStore* store, TupleHdr* hdr, const Datum* vals)
{
    /* 先计算序列化大小；t_ctid 之后再盖章，不影响 tuple 总长度。 */
    usize ser_size = compute_tuple_size(store->schema, vals);
    assert(ser_size <= HS_MAX_TUPLE_SIZE && "tuple too large for one page");

    /* 找最后一页作为追加目标；如果空间不足则创建新页。 */
    BlockSegment* seg = (BlockSegment*)segmentTree_get_last_segment(&store->tree);
    u8* page = (u8*)segment_get_data(seg);

    if (heapStore_free_space(page) < (u16)(HS_SLOT_SIZE + ser_size))
    {
        /* 新 segment 的逻辑 start 紧接上一页最后一个 slot。 */
        usize new_start = seg->base.start + seg->base.count;
        BlockSegment* ns = BlockSegment_create2(new_start);

        /* 创建页时确定真实 block_id，后续 tuple 的 ctid 直接引用它。 */
        block_id_t bid;
        if (store->block_manager != NULL)
        {
            bid = VCALL(store->block_manager, get_free_block_id);
            ns->block_manager = store->block_manager;
        }
        else
        {
            /* standalone 模式下使用本地 page_count 作为 block_id。 */
            bid = (block_id_t)store->page_count; /* sequential local fallback */
        }
        ns->block_id = bid;
        ns->block = Block_create(bid);

        /* 新页初始化后加入 SegmentTree，并更新当前追加目标。 */
        page = (u8*)segment_get_data(ns);
        heapStore_page_init(page);
        segmentTree_append_segment(&store->tree, (SegmentBase*)ns);
        seg = ns;
        store->page_count++;
    }

    /* slot_idx 由 pd_lower 推导，等于当前页已有 slot 数。 */
    u16 slot_idx = (u16)((*heapStore_pd_lower(page) - HS_BLOCK_HDR_SIZE) / HS_SLOT_SIZE);

    /* TupleHdr 中写入物理 ctid。 */
    block_id_t block_id = seg->block_id;
    hdr->t_ctid = make_item_ptr(block_id, slot_idx);

    /* 分配 slot 和 tuple 空间，并直接写入页面。 */
    u16 actual_slot;
    u8* dest = heapStore_page_reserve_slot(page, (u16)ser_size, &actual_slot);
    assert(actual_slot == slot_idx); /* must match pre-computed slot */
    serialize_tuple_into(dest, hdr, store->schema, vals);

    /* base.count 记录该 segment 的物理 slot 高水位线。 */
    seg->base.count++;
    return hdr->t_ctid;
}

//   下面的图示说明一个 HeapStore slotted page 的典型状态：
//   slot[2] 已被 vacuum 标成 LP_UNUSED，但 slot 数组位置保留。
//
//   地址 0
//   ┌──────────────────────────────────────────┐
//   │ pd_lower = 26  (6 + 5×4 = 5个slot)      │ offset=0
//   │ pd_upper = 7900                          │ offset=2
//   │ pd_flags = 0x0001 (PD_HAS_FREE_LINES)   │ offset=4
//   ├──────────────────────────────────────────┤ offset=6
//   │ slot[0]: lp_off=8100, lp_len=48  ✓alive │
//   │ slot[1]: lp_off=8052, lp_len=48  ✓alive │
//   │ slot[2]: lp_off=0,    lp_len=0   ★LP_UNUSED← 目标 │
//   │ slot[3]: lp_off=7956, lp_len=48  ✓alive │
//   │ slot[4]: lp_off=7900, lp_len=56  ✓alive │
//   ├──────────────────────────────────────────┤ offset=26 (=pd_lower)
//   │                                          │
//   │           空 闲 空 间                    │ ← rs_free_space = 7900-26 = 7874B
//   │                                          │
//   ├──────────────────────────────────────────┤ offset=7900 (=pd_upper)
//   │ tuple[4] data  56B                       │
//   │ tuple[3] data  48B                       │
//   │ (gap: tuple[2] 已被vacuum清零)           │ ← 死区（lp_off/lp_len=0,不占逻辑空间）
//   │ tuple[1] data  48B                       │
//   │ tuple[0] data  48B                       │
//   └──────────────────────────────────────────┘ offset=8192

//   ---
/* 插入一行：先尝试复用 LP_UNUSED slot，失败则追加到当前/新页面。 */
ItemPtr heapStore_insert(HeapStore* store, TxnId xid, ItemPtr emb_ctid, const Datum* values,
                         u64 null_bits)
{
    /* 构造 tuple header；t_ctid 在实际落页时由 append/reuse 路径盖章。 */
    TupleHdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.t_xmin = xid ? xid : rs_alloc_xid(store);
    hdr.t_xmax = INVALID_TXN_ID;
    hdr.t_emb_ctid = emb_ctid;
    hdr.null_bits = null_bits;
    // tuple->row_id = row_id;
    ItemPtr ctid = INVALID_ITEM_PTR;

    if (store->hint_free_seg != NULL)
    {
        /* hint_free_seg 指向可能存在 LP_UNUSED 的页面，避免每次都全表扫描。 */
        BlockSegment* scan = store->hint_free_seg;
        while (scan != NULL)
        {
            data_ptr_t page = segment_get_data(scan);
            /* 只检查带 PD_HAS_FREE_LINES 提示位的页面。 */
            if (*heapStore_pd_flags(page) & PD_HAS_FREE_LINES)
            {
                usize n_slots = (*heapStore_pd_lower(page) - HS_BLOCK_HDR_SIZE) / HS_SLOT_SIZE;
                for (usize i = 0; i < n_slots; i++)
                {
                    if (heapStore_slots(page)[i].lp_len != 0) continue; /* not LP_UNUSED */

                    /* 找到 LP_UNUSED slot 后尝试复用；如果页内空间不足会失败。 */
                    ItemPtr free_ctid = make_item_ptr(scan->block_id, (u16)i);
                    ItemPtr ctid = heapStore_reuse_slot(store, free_ctid, &hdr, values);
                    if (item_ptr_is_valid(ctid))
                    {
                        break;
                    }
                }

                /* 当前页没有可用 LP_UNUSED，清除提示位，后续插入不再优先扫描它。 */
                *heapStore_pd_flags(page) &= (u16)~PD_HAS_FREE_LINES;
            }
            scan = (BlockSegment*)scan->base.next;
        }
        if (!item_ptr_is_valid(ctid))
            store->hint_free_seg = NULL; /* no free pages remain; reset hint */
    }
    if (!item_ptr_is_valid(ctid))
    {
        /* 没有可复用 slot 时走普通 append 路径。 */
        ctid = heapStore_append_tuple(store, &hdr, values);
    }
    return ctid;
}

/* 按真实 block_id 在线性 SegmentTree 链表中查找对应页面段。 */
static BlockSegment* heapStore_find_seg_by_block(const HeapStore* store, block_id_t block_id)
{
    for (SegmentBase* s = segmentTree_get_root_segment(&store->tree); s != NULL; s = s->next)
    {
        BlockSegment* seg = (BlockSegment*)s;
        if (seg->block_id == block_id) return seg;
    }
    return NULL;
}

/* 根据 slot_idx 返回页内 tuple 起始地址；调用前需保证 slot 有效。 */
static inline u8* heapStore_page_get_tuple(u8* page, u16 slot_idx)
{
    return page + heapStore_slots(page)[slot_idx].lp_off;
}

/* 从连续字节流中读取一个 TupleVal；TEXT 会分配新字符串缓冲区。 */
static const u8* read_col_ptr(const u8* src, TupleVal* out)
{
    switch (out->type)
    {
        case TUPLE_COL_BOOL:
            /* 固定长度类型按页内格式读取后推进指针。 */
            out->v.b = *src != 0;
            return src + 1;
        case TUPLE_COL_I32:
            memcpy(&out->v.i32, src, sizeof(i32));
            return src + sizeof(i32);
        case TUPLE_COL_I64:
            memcpy(&out->v.i64, src, sizeof(i64));
            return src + sizeof(i64);
        case TUPLE_COL_F32:
            memcpy(&out->v.f32, src, sizeof(f32));
            return src + sizeof(f32);
        case TUPLE_COL_F64:
            memcpy(&out->v.f64, src, sizeof(f64));
            return src + sizeof(f64);
        case TUPLE_COL_TEXT:
        {
            /* TEXT 格式：[u32 len][bytes]，反序列化时补 '\0' 方便字符串使用。 */
            u32 len;
            memcpy(&len, src, sizeof(u32));
            out->v.text.len = len;
            out->v.text.ptr = (char*)malloc(len + 1);
            if (!out->v.text.ptr) return NULL;
            out->v.text.ptr[len] = '\0';
            memcpy(out->v.text.ptr, src + sizeof(u32), len);
            return src + sizeof(u32) + len;
        }
        default:
            return NULL; /* should not happen */
    }
}

/**
 * Deserialize one tuple from a contiguous memory pointer.
 *
 * 中文说明：从页内 tuple 指针还原 HeapTuple，包括 TupleHdr 和 Datum[]。
 * out->cols is heap-allocated (Datum[]); call row_tuple_free(out, schema) when done.
 */
static bool deserialize_tuple_from_ptr(const u8* p, const TableSchema* schema, HeapTuple* out)
{
    /* tuple 开头固定是 TupleHdr。 */
    memcpy(&out->hdr, p, sizeof(TupleHdr));
    p += sizeof(TupleHdr);

    out->ncols = schema->ncols;
    if (schema->ncols == 0)
    {
        out->cols = NULL;
        return true;
    }
    out->cols = (Datum*)calloc(schema->ncols, sizeof(Datum));
    if (!out->cols) return false;

    /* 按 schema/null_bits 解析列数据区。 */
    if (heapStore_deform_all(p, schema, out->hdr.null_bits, out->cols) != 0)
    {
        free(out->cols);
        out->cols = NULL;
        return false;
    }
    return true;
}

/**
 * Free heap-allocated column data in a HeapTuple (varlena Datums + the cols array itself).
 *
 * 中文说明：释放反序列化得到的 HeapTuple 中由本文件分配的变长列和列数组。
 */
static void row_tuple_free(HeapTuple* out, const TableSchema* schema)
{
    if (!out->cols) return;
    for (u16 i = 0; i < out->ncols && i < schema->ncols; i++)
    {
        if (out->hdr.null_bits & ((u64)1 << i)) continue;
        TupleColType ct = schema->cols[i];
        if (ct == TUPLE_COL_TEXT || ct == TUPLE_COL_JSONB)
        {
            /* 固定长度 Datum 不拥有堆内存，只有变长类型需要 free。 */
            void* ptr = DatumGetPointer(out->cols[i]);
            if (ptr) free(ptr);
        }
    }
    free(out->cols);
    out->cols = NULL;
}

/* 按物理 ctid 读取 tuple；DELETE 自指版本返回 -1，UPDATE 旧版本允许返回给调用方追链。 */
int heapStore_get_by_ctid(HeapStore* store, const TableSchema* schema, ItemPtr ctid, HeapTuple* out)
{
    /* ctid.block 定位页面，ctid.slot 定位页内 slot。 */
    block_id_t block_id = item_ptr_block_id(ctid);
    u16 slot_idx = item_ptr_slot(ctid);
    BlockSegment* seg = heapStore_find_seg_by_block(store, block_id);
    if (!seg) return -1;

    u8* page = (u8*)segment_get_data(seg);
    if ((usize)slot_idx >= seg->base.count) return -1;

    TupleSlotId* slot = &heapStore_slots(page)[slot_idx];
    if (slot->lp_len == 0) return -1; /* LP_UNUSED */

    /* slot 保存 tuple 在页内的 offset，真正数据从该位置开始。 */
    u8* tup_ptr = heapStore_page_get_tuple(page, slot_idx);

    memset(out, 0, sizeof(HeapTuple));
    out->ncols = schema->ncols;

    if (!deserialize_tuple_from_ptr(tup_ptr, schema, out)) return -1;

    // PostgreSQL 里 t_ctid 有两种状态：

    // 自指（self-pointing）：t_ctid == 自身物理地址
    //        → 要么是最新版本（alive），要么是已删除（xmax 被设置）

    // 前向（forward-pointing）：t_ctid → 另一个物理地址
    //        → UPDATE 的旧版本，指向新版本的位置
    //   ┌───────────────┬────────┬────────┬──────────────────────────────────┬────────────────────────┐
    //   │     状态      │ t_xmax │ t_ctid │               含义                │          处理 │
    //   ├───────────────┼────────┼────────┼──────────────────────────────────┼────────────────────────┤
    //   │ DELETE        │ ≠ 0    │ 自指   │ 已被删除，不可读                   │ 返回 -1 │
    //   ├───────────────┼────────┼────────┼──────────────────────────────────┼────────────────────────┤
    //   │ UPDATE 旧版本  │ ≠ 0    │ 前向   │ 指向新版本，允许 chain traversal  │
    //   正常返回，让调用方追链
    //   └───────────────┴────────┴────────┴──────────────────────────────────┴────────────────────────┘
    if (out->hdr.t_xmax != INVALID_TXN_ID)
    {
        if (item_ptr_block_id(out->hdr.t_ctid) == seg->block_id &&
            item_ptr_slot(out->hdr.t_ctid) == slot_idx)
        {
            /* 自指且 t_xmax 已设置：表示 DELETE，当前版本不可见。 */
            row_tuple_free(out, schema);
            return -1;
        }
        /* t_ctid 指向别处：表示 UPDATE 旧版本，返回给上层做版本链追踪。 */
    }
    return 0;
}

/* 按 ctid 返回页内 tuple 指针；ctid 无效或 slot 为 LP_UNUSED 时返回 NULL。 */
static u8* heapStore_lookup_ctid(HeapStore* store, ItemPtr ctid)
{
    block_id_t block_id = item_ptr_block_id(ctid);
    u16 slot_idx = item_ptr_slot(ctid);
    BlockSegment* seg = heapStore_find_seg_by_block(store, block_id);
    if (!seg) return NULL;
    if ((usize)slot_idx >= seg->base.count) return NULL;
    u8* page = (u8*)segment_get_data(seg);
    if (heapStore_slots(page)[slot_idx].lp_len == 0) return NULL;
    return heapStore_page_get_tuple(page, slot_idx);
}

// TxnId heapStore_update_by_ctid(HeapStore* store, ItemPtr old_ctid, HeapTuple* new_tuple)
// {
//     u8* old_tup = heapStore_lookup_ctid(store, old_ctid);
//     if (!old_tup) return INVALID_TXN_ID;

//     /* Verify old version is alive */
//     TupleHdr old_hdr;
//     memcpy(&old_hdr, old_tup, sizeof(TupleHdr));
//     if (old_hdr.t_xmax != INVALID_TXN_ID) return INVALID_TXN_ID;

//     TxnId txn_id = store->next_txn_id++;

//     /* Stamp new-version header (t_ctid set by rs_append_to_store). */
//     new_tuple->hdr.t_xmin = txn_id;
//     new_tuple->hdr.t_xmax = INVALID_TXN_ID;

//     ItemPtr new_ctid = heapStore_append_tuple(store, new_tuple);
//     /* new_tuple->hdr.t_ctid now holds the physical ctid of the new version. */
//     ItemPtr new_ptr = new_tuple->hdr.t_ctid; /* (new_block_id, new_slot+1) */

//     /* Re-lookup defensively: rs_append_to_store may realloc the SegmentTree
//      * node array, but block->data buffers are stable (separate allocations).
//      * The re-lookup is technically unnecessary but guards against future
//      * layout changes that inline block data into the node array. */
//     old_tup = heapStore_lookup_ctid(store, old_ctid);
//     if (old_tup)
//     {
//         memcpy(&old_hdr, old_tup, sizeof(TupleHdr));
//         old_hdr.t_xmax = txn_id;
//         old_hdr.t_ctid = new_ptr; /* forward pointer to new physical location */
//         memcpy(old_tup, &old_hdr, sizeof(TupleHdr));
//     }

//     (void)new_ctid; /* ctid is encoded in new_tuple->hdr.t_ctid for caller */
//     return txn_id;
// }

/* 按 ctid 逻辑删除 tuple：只原地修改 TupleHdr.t_xmax，不移动 tuple 数据。 */
TxnId heapStore_delete_by_ctid(HeapStore* store, ItemPtr ctid)
{
    /* DELETE 需要先定位到页内 tuple header。 */
    u8* tup = heapStore_lookup_ctid(store, ctid);
    if (!tup) return INVALID_TXN_ID;

    TupleHdr hdr;
    memcpy(&hdr, tup, sizeof(TupleHdr));
    if (hdr.t_xmax != INVALID_TXN_ID) return INVALID_TXN_ID; /* already dead */

    /* 分配删除事务号，并写入 t_xmax。 */
    TxnId txn_id = store->next_txn_id++;

    /* 原地更新 TupleHdr：t_ctid 保持自指，表示 DELETE 而非 UPDATE forward。 */
    hdr.t_xmax = txn_id;
    memcpy(tup, &hdr, sizeof(TupleHdr));

    return txn_id;
}

/* 开始 HeapStore 顺序扫描；持有 SHARED 锁直到 heapStoreIter_end。 */
void heapStoreIter_begin(HeapStoreIter* iter, HeapStore* store)
{
    LWLockAcquire(&store->lock, LW_SHARED);
    /* 从 SegmentTree 根 segment 和第 0 个 slot 开始扫描。 */
    iter->store = store;
    iter->curr_seg = segmentTree_get_root_segment(&store->tree);
    iter->slot_idx = 0;
}

/* 返回下一个非 LP_UNUSED tuple header；列数据地址通过 iter->curr_col_data 暴露。 */
const TupleHdr* heapStoreIter_next(HeapStoreIter* iter)
{
    while (iter->curr_seg != NULL)
    {
        if ((usize)iter->slot_idx >= iter->curr_seg->count)
        {
            /* 当前 segment 扫完后切到下一个页面段。 */
            iter->curr_seg = iter->curr_seg->next;
            iter->slot_idx = 0;
            continue;
        }

        BlockSegment* bseg = (BlockSegment*)iter->curr_seg;
        u8* page = (u8*)segment_get_data(bseg);
        u32 slot = iter->slot_idx++;

        TupleSlotId* sl = &heapStore_slots(page)[slot];
        if (sl->lp_len == 0) continue; /* LP_UNUSED */
        const TupleHdr* hdr = (const TupleHdr*)heapStore_page_get_tuple(page, (u16)slot);
        /* 暴露 tuple 长度和列数据页内指针；只要 SHARED 锁未释放，该指针保持有效。 */
        iter->curr_tup_len = sl->lp_len;
        iter->curr_col_data = (const u8*)hdr + sizeof(TupleHdr);
        return hdr;
    }
    return NULL;
}

/* 结束顺序扫描并释放 begin 时持有的 SHARED 锁。 */
void heapStoreIter_end(HeapStoreIter* iter)
{
    if (iter->store)
    {
        LWLockRelease(&iter->store->lock);
        iter->store = NULL;
    }
}
