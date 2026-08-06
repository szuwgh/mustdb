#ifndef TYPES_H
#define TYPES_H
#include "mustdb_type.h"
#include "vector.h"
#include "interface.h"

// internal types
typedef enum
{
    TYPE_INVALID = 0,
    TYPE_INT8 = 1,
    TYPE_INT16 = 2,
    TYPE_INT32 = 3,
    TYPE_INT64 = 4,
    TYPE_FLOAT32 = 5,
    TYPE_FLOAT64 = 6,
} TypeID;

usize get_typeid_size(TypeID type);

// 列向量
typedef struct
{
    TypeID type;
    usize count;
    data_ptr_t data;
} MustDbVector;

void MustDbVector_init(MustDbVector* vector, TypeID type);

void MustDbVector_from_vector(MustDbVector* vector, Vector src, TypeID type);

void MustDbVector_deinit(MustDbVector* vector);

usize MustDbVector_size(MustDbVector* vector);

data_ptr_t MustDbVector_get_data(MustDbVector* vector);

#endif