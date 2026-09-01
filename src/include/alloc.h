#ifndef ALLOC_H
#define ALLOC_H

#include "must.h"
#include <stdatomic.h>

typedef struct MAllocStats
{
    atomic_uint_fast64_t alloc_count; /*分配次数*/
    atomic_uint_fast64_t free_count; /*free 调用次数*/
    atomic_uint_fast64_t realloc_count; /*realloc 调用次数*/
    atomic_uint_fast64_t bytes_live; /*仍存活的用户分配字节数*/
    atomic_uint_fast64_t bytes_peak; /* 上次重置统计后观察到的 bytes_live 峰值。 */
} MAllocStats;

typedef struct
{
    u64 alloc_count; /*分配次数*/
    u64 free_count; /*free 调用次数*/
    u64 realloc_count;
    u64 bytes_live;
    u64 bytes_peak;
} MAllocStatsSnap;

void* mmalloc(usize size);
void* mcalloc(usize count, usize size);
void* mrealloc(void* ptr, usize size);
void mfree(void* ptr);

MAllocStatsSnap get_mem_stats(void);
void set_mem_fault_countdown(i64 countdown);
void clear_mem_fault(void);

#endif
