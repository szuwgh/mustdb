#include "alloc.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUSTDB_ENABLE_ALLOC_FAULTS
#define MUSTDB_ENABLE_ALLOC_FAULTS 0
#endif

static atomic_int_fast64_t fault_countdown = ATOMIC_VAR_INIT(-1);

typedef struct
{
    /* data */
    usize size; /*用来做统计*/
} AllocHdr;

static MAllocStats global_mem_stats;

/* 在 bytes_live 增长后刷新峰值。多线程下可能同时更新 peak，所以用 CAS
 * 只在当前 live 大于旧 peak 时尝试写入，失败则读取竞争线程写回的新值继续判断。
 */
static void update_mem_peak(u64 live)
{
    u64 peak = atomic_load_explicit(&global_mem_stats.bytes_peak, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&global_mem_stats.bytes_peak, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

/* 测试用故障注入判断：
 * - countdown < 0：关闭故障注入；
 * - countdown > 0：本次分配成功，并把倒计时原子减 1；
 * - countdown == 0：本次及后续分配失败，直到 clear_mem_fault()。
 *
 * 这里用 CAS 而不是普通自减，是为了多线程测试中精确控制“前 N 次分配成功”。
 */
static bool mem_should_fail(void)
{
    i64 countdown = atomic_load_explicit(&fault_countdown, memory_order_relaxed);
    for (;;)
    {
        if (countdown < 0) return false;
        if (countdown == 0) return true;
        if (atomic_compare_exchange_weak_explicit(&fault_countdown, &countdown, countdown - 1,
                                                  memory_order_relaxed, memory_order_relaxed))
            return false;
    }
}

/* 记录一次成功的 malloc/calloc，并增加当前 live 字节数。 */
static void alloc_mem_stats(usize size)
{
    atomic_fetch_add_explicit(&global_mem_stats.alloc_count, 1, memory_order_relaxed);
    u64 live =
        atomic_fetch_add_explicit(&global_mem_stats.bytes_live, (u64)size, memory_order_relaxed) +
        (u64)size;
    update_mem_peak(live);
}

/* 从 bytes_live 中扣减 size。这里做饱和减法，避免 debug/测试路径重复释放时
 * 统计值下溢成一个很大的 u64。
 */
static void sub_mem_stats_live(usize size)
{
    u64 old_live = atomic_load_explicit(&global_mem_stats.bytes_live, memory_order_relaxed);
    for (;;)
    {
        u64 new_live = old_live >= (u64)size ? old_live - (u64)size : 0;
        if (atomic_compare_exchange_weak_explicit(&global_mem_stats.bytes_live, &old_live, new_live,
                                                  memory_order_relaxed, memory_order_relaxed))
            return;
    }
}

static void free_mem_stats(usize size)
{
    atomic_fetch_add_explicit(&global_mem_stats.free_count, 1, memory_order_relaxed);
    sub_mem_stats_live(size); /*释放时扣减 live 字节数*/
}

/* 分配 size 字节用户内存。
 * 返回给调用方的是 header 之后的地址；header 只由 allocator 内部使用。
 */
void* mmalloc(usize size)
{
#if MUSTDB_ENABLE_ALLOC_FAULTS
    if (mem_should_fail()) return NULL;
#endif
    if (size > ((usize)-1) - sizeof(AllocHdr)) return NULL; /*防止溢出*/
    usize total = sizeof(AllocHdr) + size;
    AllocHdr* hdr = (AllocHdr*)malloc(total);
    if (!hdr) return NULL;
    hdr->size = size;
    alloc_mem_stats(size);
    return (void*)(hdr + 1);
}

/* 分配 count * size 字节并清零；先检查乘法溢出，再复用 mustdb_malloc。 */
void* mcalloc(usize count, usize size)
{
    if (count != 0 && size > ((usize)-1) / count) return NULL; /*防止溢出*/
    usize bytes = count * size;
    void* ptr = mmalloc(bytes);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

void* mrealloc(void* ptr, usize size)
{
    if (!ptr) return mmalloc(size);
    if (size == 0)
    {
        mfree(ptr);
        return NULL;
    }
#if MUSTDB_ENABLE_ALLOC_FAULTS
    if (mem_should_fail()) return NULL;
#endif
    AllocHdr* old_hdr = ((AllocHdr*)ptr) - 1;
    usize old_size = old_hdr->size;
    if (size > ((usize)-1) - sizeof(AllocHdr)) return NULL;
    usize total = sizeof(AllocHdr) + size;
    AllocHdr* new_hdr = (AllocHdr*)realloc(old_hdr, total);
    if (!new_hdr) return NULL;
    new_hdr->size = size;
    atomic_fetch_add_explicit(&global_mem_stats.realloc_count, 1, memory_order_relaxed);
    if (size >= old_size)
    {
        u64 delta = (u64)(size - old_size);
        u64 live =
            atomic_fetch_add_explicit(&global_mem_stats.bytes_live, delta, memory_order_relaxed) +
            delta;
        update_mem_peak(live);
    }
    else
    {
        sub_mem_stats_live(old_size - size);
    }
    return (void*)(new_hdr + 1);
}

void mfree(void* ptr)
{
    if (!ptr) return;
    AllocHdr* hdr = ((AllocHdr*)ptr) - 1;
    usize size = hdr->size;
    free_mem_stats(size);
    free(hdr);
}

/* 读取 allocator 统计快照。返回值使用普通 u64，调用方不需要接触原子类型。 */
MAllocStatsSnap get_mem_stats(void)
{
    MAllocStatsSnap stats;
    stats.alloc_count = atomic_load_explicit(&global_mem_stats.alloc_count, memory_order_relaxed);
    stats.free_count = atomic_load_explicit(&global_mem_stats.free_count, memory_order_relaxed);
    stats.realloc_count =
        atomic_load_explicit(&global_mem_stats.realloc_count, memory_order_relaxed);
    stats.bytes_live = atomic_load_explicit(&global_mem_stats.bytes_live, memory_order_relaxed);
    stats.bytes_peak = atomic_load_explicit(&global_mem_stats.bytes_peak, memory_order_relaxed);
    return stats;
}

/* 设置故障注入倒计时。countdown == 0 表示下一次分配立即失败；
 * countdown < 0 表示关闭故障注入。
 */
void set_mem_fault_countdown(i64 countdown)
{
#if MUSTDB_ENABLE_ALLOC_FAULTS
    atomic_store_explicit(&fault_countdown, countdown, memory_order_relaxed);
#else
    (void)countdown;
#endif
}

/* 关闭故障注入，让后续分配恢复正常。 */
void clear_mem_fault(void)
{
#if MUSTDB_ENABLE_ALLOC_FAULTS
    atomic_store_explicit(&fault_countdown, -1, memory_order_relaxed);
#endif
}
