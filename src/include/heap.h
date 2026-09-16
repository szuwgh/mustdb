#ifndef HEAP_H
#define HEAP_H

#include "must.h"

/* ============================================================
 * Datum — PostgreSQL-style unified value representation (u64).
 *
 * By-value types (BOOL, INT32, INT64, FLOAT32, FLOAT64):
 *   Stored inline in the low bits of the u64.
 *
 * By-reference types (TEXT, BYTEA, JSONB):
 *   Stored as pointer (PointerGetDatum), in-memory layout:
 *     TEXT / BYTEA: [u32 content_len][data bytes...]
 *     JSONB:        MustDbJsonb* — vl_len_ bytes total (includes 4-byte header)
 *
 * null_bits (u64 in TupleHdr): bit i set → col i is NULL → Datum[i] undefined.
 * ============================================================ */

typedef u64 Datum;

/*列的类型定义*/
typedef enum
{
    TUPLE_COL_NULL = 0,  /* null / no value */
    TUPLE_COL_BOOL = 1,
    TUPLE_COL_I32 = 2,
    TUPLE_COL_I64 = 3,
    TUPLE_COL_F32 = 4,
    TUPLE_COL_F64 = 5,
    TUPLE_COL_TEXT = 6,
    TUPLE_COL_JSONB = 7,
    TUPLE_COL_VECTOR = 9,
} TupleColType;

typedef struct
{
    const char* name; /* 列名；学习 PG attname 语义 */
    TupleColType type; /* 列类型；学习 PG atttypid 的简化版 */
} TupleColDef;

typedef struct
{
    /* data */
    const TupleColDef* cols;  /* borrowed TupleColDef[] */
    u16 ncols; /* tuple natts；当前最多通过 TupleHdr.null_bits 支持 64 列 */
} HeapSchema;

typedef struct
{
    /* data */
    u16 ip_blkid_hi; /*Block ID 高 16 位*/
    u16 ip_blkid_lo; /*Block ID 低 16 位*/
    u16 ip_posid;    /* 页内 slot 编号 */
} ItemPtr;

typedef struct HeapStore
{
    const HeapSchema* schema;
} HeapStore;

typedef struct HeapStoreContext
{
    const HeapSchema* schema;
    u64 heap_fork_id;
} HeapStoreContext;

/*

*/
ItemPtr heap_insert(HeapStore* heap, TxnId xid, u32 cid, ItemPtr emb_ctid, const Datum* values,
                    u64 null_bits);

int heap_create(HeapStore* store, const HeapStoreContext* ctx);

int heap_open(HeapStore* store, const HeapStoreContext* ctx);
#endif /* HEAP_H */