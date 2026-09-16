#include "vector_types.h"
#include <stdlib.h>
#include <string.h>

usize get_typeid_size(TypeID type)
{
    switch (type)
    {
        case TYPE_INT8:
            return sizeof(u8);
        case TYPE_INT16:
            return sizeof(i16);
        case TYPE_INT32:
            return sizeof(i32);
        case TYPE_INT64:
            return sizeof(i64);
        case TYPE_FLOAT32:
            return sizeof(f32);
        case TYPE_FLOAT64:
            return sizeof(f64);
        default:
            return 0;
    }
}

void MustDbVector_init(MustDbVector* vector, TypeID type)
{
    vector->type = type;
    vector->count = 0;
    vector->data = NULL;
}

void MustDbVector_from_slice(MustDbVector* vector, Slice src, TypeID type)
{
    vector->type = type;
    vector->count = src.size;
    vector->data = src.data;
}

void MustDbVector_deinit(MustDbVector* vector)
{
    vector->count = 0;
    free(vector->data);
}

usize MustDbVector_size(MustDbVector* vector)
{
    return vector->count;
}

data_ptr_t MustDbVector_get_data(MustDbVector* vector)
{
    return vector->data;
}
