#ifndef HEAP_H
#define HEAP_H

#include "must.h"

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
} TupleSchema;

typedef struct
{
    /* data */
} ItemPtr;

#endif /* HEAP_H */