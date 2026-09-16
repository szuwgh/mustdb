#ifndef STORE_H
#define STORE_H

#include "segment.h"
#include "heap.h"
#include "must.h"
#include "lock.h"

typedef struct TableSchema TableSchema;

/* Forward declaration — full definition is in datatable.h.
 * Pointer-only use here; datatable.h provides the struct body.
 * Avoids the circular: datatable.h → store.h → datatable.h. */
typedef struct DataChunk DataChunk;

/* ============================================================
 * heap store
 * ============================================================*/

#define INVALID_ITEM_PTR \
    ((ItemPtr){.ip_blkid_hi = UINT16_MAX, .ip_blkid_lo = UINT16_MAX, .ip_posid = UINT16_MAX})

#define INVALID_TXN_ID    ((TxnId)0)  /* 未设置 / 占位 */

#define HS_BLOCK_HDR_SIZE 6u /* sizeof(pd_lower) + sizeof(pd_upper) + sizeof(pd_flags) */
#define HS_SLOT_SIZE      4u /* sizeof(RowSlotId) */
/** PG-compatible page flag: set when page has at least one LP_UNUSED slot. */
#define PD_HAS_FREE_LINES 0x0001u

// Max tuple size in heap store
#define HS_MAX_TUPLE_SIZE ((usize)(BLOCK_SIZE - HS_BLOCK_HDR_SIZE - HS_SLOT_SIZE))

// typedef enum
// {
//     TUPLE_COL_NULL = 0,  /* null / no value */
//     TUPLE_COL_BOOL = 1,
//     TUPLE_COL_I32 = 2,
//     TUPLE_COL_I64 = 3,
//     TUPLE_COL_F32 = 4,
//     TUPLE_COL_F64 = 5,
//     TUPLE_COL_TEXT = 6,
//     TUPLE_COL_JSONB = 7,
// } TupleColType;

typedef struct
{
    TupleColType type;
    union
    {
        bool b;
        i32 i32;
        i64 i64;
        f32 f32;
        f64 f64;
        struct
        {
            char* ptr;
            u32 len;
        } text;    /* heap-allocated when from row_store_get */
       // JsonB* jsonb;   /* heap-allocated when from row_store_get */
    } v;
} TupleVal;

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

typedef u64 Datum;  // 每个字段的数据载体

/* ---- By-value → Datum ---- */
#define BoolGetDatum(v)  ((Datum)(u8)(!!(v)))
#define Int32GetDatum(v) ((Datum)(u64)(i64)(i32)(v))
#define Int64GetDatum(v) ((Datum)(u64)(i64)(v))
static inline Datum Float32GetDatum(f32 v)
{
    u32 b;
    memcpy(&b, &v, 4);
    return (Datum)b;
}
static inline Datum Float64GetDatum(f64 v)
{
    u64 b;
    memcpy(&b, &v, 8);
    return b;
}

/* ---- Datum → by-value ---- */
#define DatumGetBool(d)  ((bool)((u8)(d) != 0))
#define DatumGetInt32(d) ((i32)(i64)(u64)(d))
#define DatumGetInt64(d) ((i64)(u64)(d))
static inline f32 DatumGetFloat32(Datum d)
{
    u32 b = (u32)(d);
    f32 v;
    memcpy(&v, &b, 4);
    return v;
}
static inline f64 DatumGetFloat64(Datum d)
{
    u64 b = (u64)(d);
    f64 v;
    memcpy(&v, &b, 8);
    return v;
}

/* ---- By-reference ---- */
#define PointerGetDatum(p) ((Datum)(uintptr_t)(const void*)(p))
#define DatumGetPointer(d) ((void*)(uintptr_t)(d))
#define DatumGetJsonb(d)   ((MustDbJsonb*)DatumGetPointer(d))
#define JsonbGetDatum(jb)  PointerGetDatum(jb)

typedef u32 CommandId;

typedef struct
{
    u16 ip_blkid_hi;  /* 2: high 16 bits of disk block_id (PG bi_hi)              */
    u16 ip_blkid_lo;  /* 2: low  16 bits of disk block_id (PG bi_lo)              */
    u16 ip_posid;     /* 2: 1-based slot number within the page (PG OffsetNumber) */
} ItemPtr;            /* 6 bytes, alignment 2 — matches PG ItemPointerData */

typedef struct
{
    TxnId t_xmin;      /*  0: inserting transaction                           */
    TxnId t_xmax;      /*  8: deleting/updating transaction; 0 = alive        */
    ItemPtr t_emb_ctid;  /* 16: EmbeddingStore ctid (INVALID_ITEM_PTR if none)  */
    ItemPtr t_ctid;      /* 22: heap physical location or UPDATE forward ptr    */
    u64 null_bits;   /* 32: bit i set → user col i is NULL                  */
} TupleHdr; /* 40 bytes: 8+8+4+6+2+2+1+1+8 = 40                  */

typedef struct
{
    u16 lp_off;  /* 2: byte offset of the tuple data within the page */
    u16 lp_len;  /* 2: length of the tuple data in bytes */
} TupleSlotId;

typedef struct
{
    TupleHdr hdr; /* MVCC header (written by row_store_*; read back by get) */
    Datum* cols; /* array of ncols TupleVal (caller-allocated for insert/update) */
    u16 ncols; /* number of columns (must match schema->ncols) */
} HeapTuple;

#endif
