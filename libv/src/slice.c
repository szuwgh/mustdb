#include "slice.h"
#include <stdlib.h>
#include <string.h>

#define SLICE_DEFAULT_CAPACITY 16
#define SLICE_GROWTH_FACTOR    2

// 获取指定索引元素的内部指针（辅助函数）
static inline void* slice_get_ptr(const Slice* slice, usize index)
{
    return (char*)slice->data + (index * slice->element_size);
}

int slice_init(Slice* slice, usize element_size, usize initial_capacity)
{
    if (initial_capacity == 0)
    {
        initial_capacity = SLICE_DEFAULT_CAPACITY;
    }

    slice->data = malloc(element_size * initial_capacity);
    if (!slice->data)
    {
        return -1;
    }

    slice->size = 0;
    slice->capacity = initial_capacity;
    slice->element_size = element_size;

    return 0;
}

Slice* slice_create(usize element_size, usize initial_capacity)
{
    if (element_size == 0)
    {
        return NULL;  // 元素大小不能为0
    }

    Slice* slice = (Slice*)malloc(sizeof(Slice));
    if (!slice)
    {
        return NULL;
    }

    if (slice_init(slice, element_size, initial_capacity) != 0)
    {
        free(slice);
        return NULL;
    }

    return slice;
}

void slice_destroy(Slice* slice)
{
    if (slice)
    {
        if (slice->data)
        {
            free(slice->data);
        }
        free(slice);
    }
}

void slice_deinit(Slice* slice)
{
    if (slice)
    {
        if (slice->data)
        {
            free(slice->data);
            slice->data = NULL;
        }
        slice->size = 0;
        slice->capacity = 0;
        slice->element_size = 0;
    }
}

static int slice_grow(Slice* slice)
{
    usize new_capacity =
        slice->capacity > 0 ? slice->capacity * SLICE_GROWTH_FACTOR : SLICE_DEFAULT_CAPACITY;
    void* new_data = realloc(slice->data, slice->element_size * new_capacity);
    if (!new_data)
    {
        return -1;
    }

    slice->data = new_data;
    slice->capacity = new_capacity;
    return 0;
}

int slice_push_back(Slice* slice, const void* element)
{
    if (!slice || !element)
    {
        return -1;
    }

    if (slice->size >= slice->capacity)
    {
        if (slice_grow(slice) != 0)
        {
            return -1;
        }
    }

    // 复制元素到缓冲区末尾
    void* dest = slice_get_ptr(slice, slice->size);
    memcpy(dest, element, slice->element_size);
    slice->size++;
    return 0;
}

int slice_pop_back(Slice* slice, void* out)
{
    if (!slice || slice->size == 0)
    {
        return -1;
    }

    slice->size--;

    // 如果需要输出，复制元素
    if (out)
    {
        void* src = slice_get_ptr(slice, slice->size);
        memcpy(out, src, slice->element_size);
    }

    return 0;
}

void* slice_get(const Slice* slice, usize index)
{
    if (!slice || index >= slice->size)
    {
        return NULL;
    }

    return slice_get_ptr(slice, index);
}

int slice_get_copy(const Slice* slice, usize index, void* out)
{
    if (!slice || !out || index >= slice->size)
    {
        return -1;
    }

    void* src = slice_get_ptr(slice, index);
    memcpy(out, src, slice->element_size);
    return 0;
}

int slice_set(Slice* slice, usize index, const void* element)
{
    if (!slice || !element || index >= slice->size)
    {
        return -1;
    }

    void* dest = slice_get_ptr(slice, index);
    memcpy(dest, element, slice->element_size);
    return 0;
}

usize slice_size(const Slice* slice)
{
    if (!slice)
    {
        return 0;
    }
    return slice->size;
}

usize slice_capacity(const Slice* slice)
{
    if (!slice)
    {
        return 0;
    }
    return slice->capacity;
}

usize slice_element_size(const Slice* slice)
{
    if (!slice)
    {
        return 0;
    }
    return slice->element_size;
}

bool slice_empty(const Slice* slice)
{
    return slice == NULL || slice->size == 0;
}

void slice_clear(Slice* slice)
{
    if (slice)
    {
        slice->size = 0;
    }
}

int slice_reserve(Slice* slice, usize new_capacity)
{
    if (!slice)
    {
        return -1;
    }

    if (new_capacity <= slice->capacity)
    {
        return 0;  // 已经有足够容量
    }

    void* new_data = realloc(slice->data, slice->element_size * new_capacity);
    if (!new_data)
    {
        return -1;
    }

    slice->data = new_data;
    slice->capacity = new_capacity;
    return 0;
}

int slice_resize(Slice* slice, usize new_size, const void* default_value)
{
    if (!slice)
    {
        return -1;
    }

    if (new_size > slice->capacity)
    {
        if (slice_reserve(slice, new_size) != 0)
        {
            return -1;
        }
    }

    if (new_size > slice->size)
    {
        // 扩大：填充默认值或零
        for (usize i = slice->size; i < new_size; i++)
        {
            void* dest = slice_get_ptr(slice, i);
            if (default_value)
            {
                memcpy(dest, default_value, slice->element_size);
            }
            else
            {
                memset(dest, 0, slice->element_size);
            }
        }
    }

    slice->size = new_size;
    return 0;
}

int slice_insert(Slice* slice, usize index, const void* element)
{
    if (!slice || !element || index > slice->size)
    {
        return -1;
    }

    if (slice->size >= slice->capacity)
    {
        if (slice_grow(slice) != 0)
        {
            return -1;
        }
    }

    // 移动元素为新元素腾出空间
    if (index < slice->size)
    {
        void* src = slice_get_ptr(slice, index);
        void* dest = slice_get_ptr(slice, index + 1);
        usize bytes_to_move = (slice->size - index) * slice->element_size;
        memmove(dest, src, bytes_to_move);
    }

    // 复制新元素
    void* dest = slice_get_ptr(slice, index);
    memcpy(dest, element, slice->element_size);
    slice->size++;
    return 0;
}

int slice_erase(Slice* slice, usize index, void* out)
{
    if (!slice || index >= slice->size)
    {
        return -1;
    }

    // 如果需要输出，复制元素
    if (out)
    {
        void* src = slice_get_ptr(slice, index);
        memcpy(out, src, slice->element_size);
    }

    // 向前移动元素
    if (index < slice->size - 1)
    {
        void* dest = slice_get_ptr(slice, index);
        void* src = slice_get_ptr(slice, index + 1);
        usize bytes_to_move = (slice->size - index - 1) * slice->element_size;
        memmove(dest, src, bytes_to_move);
    }

    slice->size--;
    return 0;
}

ssize_t slice_find(const Slice* slice, const void* element,
                    int (*compare)(const void*, const void*))
{
    if (!slice || !element || !compare)
    {
        return -1;
    }

    for (usize i = 0; i < slice->size; i++)
    {
        void* current = slice_get_ptr(slice, i);
        if (compare(current, element) == 0)
        {
            return (ssize_t)i;
        }
    }

    return -1;
}

void* slice_front(const Slice* slice)
{
    if (!slice || slice->size == 0)
    {
        return NULL;
    }
    return slice->data;
}

void* slice_back(const Slice* slice)
{
    if (!slice || slice->size == 0)
    {
        return NULL;
    }
    return slice_get_ptr(slice, slice->size - 1);
}

void* slice_data(const Slice* slice)
{
    if (!slice)
    {
        return NULL;
    }
    return slice->data;
}

void slice_iter_init(SliceIterator* iter, Slice* slice)
{
    if (!iter || !slice)
    {
        return;
    }
    iter->slice = slice;
    iter->index = (usize)-1;  // "before-first" position
}

bool slice_iter_next(SliceIterator* iter)
{
    if (!iter || !iter->slice)
    {
        return false;
    }
    iter->index++;
    return iter->index < iter->slice->size;
}

void* slice_iter_get(SliceIterator* iter)
{
    if (!iter || !iter->slice || iter->index >= iter->slice->size)
    {
        return NULL;
    }
    return slice_get_ptr(iter->slice, iter->index);
}
