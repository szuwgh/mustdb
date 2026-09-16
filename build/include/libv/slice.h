#ifndef SLICE_H
#define SLICE_H

#include "type.h"
#include <sys/types.h>

/**
 * Slice - 拥有底层内存、可自动扩容的 Go 风格泛型切片
 *
 * 特性：
 * - 值拷贝语义：存储数据的副本，而非引用
 * - 自动扩容：容量不足时自动增长（2倍）
 * - 类型安全：通过 element_size 指定类型大小
 * - 支持任意类型：基本类型、结构体、指针等
 *
 * 使用示例：
 *
 * 1. 存储基本类型：
 *    Slice* slice = slice_create(sizeof(int), 0);
 *    int val = 42;
 *    slice_push_back(slice, &val);  // 复制值
 *    int* ptr = (int*)slice_get(slice, 0);
 *
 * 2. 存储结构体：
 *    Slice* slice = slice_create(sizeof(MyStruct), 0);
 *    MyStruct s = {...};
 *    slice_push_back(slice, &s);  // 复制整个结构体
 *
 * 3. 存储指针（需要手动管理内存）：
 *    Slice* slice = slice_create(sizeof(int*), 0);
 *    int* ptr = malloc(sizeof(int));
 *    *ptr = 42;
 *    slice_push_back(slice, &ptr);  // 复制指针的值（地址）
 *
 *    // 使用指针
 *    int** ptr_ptr = (int**)slice_get(slice, 0);
 *    int* retrieved = *ptr_ptr;
 *    printf("%d", *retrieved);  // 42
 *
 *    // 清理：必须手动释放指针指向的内存
 *    for (size_t i = 0; i < slice_size(slice); i++) {
 *        int** p = (int**)slice_get(slice, i);
 *        free(*p);
 *    }
 *    slice_destroy(slice);
 *
 * 4. 存储 void* 通用指针：
 *    Slice* slice = slice_create(sizeof(void*), 0);
 *    void* ptr = some_data;
 *    slice_push_back(slice, &ptr);
 */

// 动态数组结构体 - 泛型实现
typedef struct
{
    void* data;          // 数据缓冲区（存储实际数据，非指针）
    usize size;          // 当前元素数量
    usize capacity;      // 当前容量
    usize element_size;  // 每个元素的字节大小
} Slice;

typedef struct
{
    Slice* slice;
    usize index;
} SliceIterator;

/**
 * @brief 初始化Slice
 *
 * @param slice Slice指针
 * @param element_size 每个元素的字节大小（例如 sizeof(int)）
 * @param initial_capacity 初始容量，如果为0则使用默认容量
 */
int slice_init(Slice* slice, usize element_size, usize initial_capacity);

/* 栈上创建一个 owning Slice；使用 slice_deinit() 释放底层内存。 */
#define SLICE(element_type, ...)                                      \
    ({                                                                \
        Slice _slice = {0};                                           \
        slice_init(&_slice, sizeof(element_type), ##__VA_ARGS__);     \
        _slice;                                                       \
    })

/**
 * @brief 创建一个新的Slice
 *
 * @param element_size 每个元素的字节大小（例如 sizeof(int)）
 * @param initial_capacity 初始容量，如果为0则使用默认容量
 * @return Slice* 新创建的Slice指针，失败返回NULL
 */
Slice* slice_create(usize element_size, usize initial_capacity);

/**
 * @brief 销毁Slice并释放所有资源
 *
 * @param slice 要销毁的Slice指针
 */
void slice_destroy(Slice* slice);

/**
 * @brief 销毁Slice并释放所有资源
 *
 * @param slice 要销毁的Slice指针
 */
void slice_deinit(Slice* slice);

/**
 * @brief 向Slice末尾添加元素（复制）
 *
 * @param slice Slice指针
 * @param element 要添加的元素指针（会被复制）
 * @return int 成功返回0，失败返回-1
 */
int slice_push_back(Slice* slice, const void* element);

/**
 * @brief 移除Slice末尾的元素
 *
 * @param slice Slice指针
 * @param out 输出参数，存储被移除的元素（可以为NULL）
 * @return int 成功返回0，失败返回-1
 */
int slice_pop_back(Slice* slice, void* out);

/**
 * @brief 获取指定索引的元素
 *
 * @param slice Slice指针
 * @param index 元素索引
 * @return void* 元素指针（指向内部数据），如果索引越界返回NULL
 */
void* slice_get(const Slice* slice, usize index);

/**
 * @brief 设置指定索引的元素（复制）
 *
 * @param slice Slice指针
 * @param index 元素索引
 * @param element 新元素指针（会被复制）
 * @return int 成功返回0，失败返回-1
 */
int slice_set(Slice* slice, usize index, const void* element);

/**
 * @brief 获取Slice的大小
 *
 * @param slice Slice指针
 * @return usize 元素数量
 */
usize slice_size(const Slice* slice);

/**
 * @brief 获取Slice的容量
 *
 * @param slice Slice指针
 * @return usize 当前容量
 */
usize slice_capacity(const Slice* slice);

/**
 * @brief 获取元素大小
 *
 * @param slice Slice指针
 * @return usize 每个元素的字节大小
 */
usize slice_element_size(const Slice* slice);

/**
 * @brief 检查Slice是否为空
 *
 * @param slice Slice指针
 * @return bool true表示为空，false表示非空
 */
bool slice_empty(const Slice* slice);

/**
 * @brief 清空Slice中的所有元素
 *
 * @param slice Slice指针
 */
void slice_clear(Slice* slice);

/**
 * @brief 预留容量
 *
 * @param slice Slice指针
 * @param new_capacity 新容量
 * @return int 成功返回0，失败返回-1
 */
int slice_reserve(Slice* slice, usize new_capacity);

/**
 * @brief 调整Slice大小
 *
 * @param slice Slice指针
 * @param new_size 新大小
 * @param default_value 新元素的默认值（如果扩大，可以为NULL表示零初始化）
 * @return int 成功返回0，失败返回-1
 */
int slice_resize(Slice* slice, usize new_size, const void* default_value);

/**
 * @brief 在指定位置插入元素（复制）
 *
 * @param slice Slice指针
 * @param index 插入位置
 * @param element 要插入的元素指针（会被复制）
 * @return int 成功返回0，失败返回-1
 */
int slice_insert(Slice* slice, usize index, const void* element);

/**
 * @brief 移除指定位置的元素
 *
 * @param slice Slice指针
 * @param index 要移除的位置
 * @param out 输出参数，存储被移除的元素（可以为NULL）
 * @return int 成功返回0，失败返回-1
 */
int slice_erase(Slice* slice, usize index, void* out);

/**
 * @brief 查找元素
 *
 * @param slice Slice指针
 * @param element 要查找的元素指针
 * @param compare 比较函数，返回0表示相等
 * @return ssize_t 元素索引，未找到返回-1
 */
ssize_t slice_find(const Slice* slice, const void* element,
                    int (*compare)(const void*, const void*));

/**
 * @brief 获取Slice的前端元素
 *
 * @param slice Slice指针
 * @return void* 前端元素指针（指向内部数据），如果为空返回NULL
 */
void* slice_front(const Slice* slice);

/**
 * @brief 获取Slice的后端元素
 *
 * @param slice Slice指针
 * @return void* 后端元素指针（指向内部数据），如果为空返回NULL
 */
void* slice_back(const Slice* slice);

/**
 * @brief 获取指定索引的元素（带边界检查，复制到输出）
 *
 * @param slice Slice指针
 * @param index 元素索引
 * @param out 输出缓冲区
 * @return int 成功返回0，失败返回-1
 */
int slice_get_copy(const Slice* slice, usize index, void* out);

/**
 * @brief 获取内部数据指针（用于直接访问，小心使用）
 *
 * @param slice Slice指针
 * @return void* 内部数据缓冲区指针
 */
void* slice_data(const Slice* slice);

/**
 * @brief 初始化Slice迭代器
 *
 * @param iter Slice迭代器指针
 * @param slice Slice指针
 */
void slice_iter_init(SliceIterator* iter, Slice* slice);

/**
 * @brief 移动Slice迭代器到下一个元素
 *
 * @param iter Slice迭代器指针
 * @return bool true表示成功移动，false表示到达末尾
 */
bool slice_iter_next(SliceIterator* iter);

/**
 * @brief 获取当前Slice迭代器指向的元素
 *
 * @param iter Slice迭代器指针
 * @return void* 元素指针（指向内部数据），如果迭代器无效返回NULL
 */
void* slice_iter_get(SliceIterator* iter);

// 类型安全的宏辅助（可选）
#define SLICE_GET(slice, index, type) ((type*)slice_get(slice, index))
#define SLICE_AT(slice, index, type)  (*SLICE_GET(slice, index, type))

#define SLICE_FRONT(slice, type)      ((type*)slice_front(slice))

#define SLICE_BACK(slice, type)       ((type*)slice_back(slice))

#define SLICE_FOREACH(slice_ptr, entry_var)         \
    SliceIterator _iter_##entry_var;                \
    slice_iter_init(&_iter_##entry_var, (slice_ptr)); \
    void* entry_var;                                 \
    while (slice_iter_next(&_iter_##entry_var) &&   \
           ((entry_var = slice_iter_get(&_iter_##entry_var)), 1))

#endif  // SLICE_H
