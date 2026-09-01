#include "pool.h"
#include "stdatomic.h"
#include <stdlib.h>

/* hash_table 的最小 bucket 数。
 * 实际 bucket 数会扩展到 >= num_pages * 2 的 2 的幂，降低冲突概率。 */
#define BUFFER_POOL_HASH_MIN        16u
#define BUFFER_POOL_USAGE_COUNT_MAX 5u

typedef struct BufferPolicyOps
{
    /*BufferPool destroy 时调用 */

    void (*destroy)(void* impl);
} BufferPolicyOps;

/* 根据 BufferPool 容量计算 hash bucket 数。
 * 取 2 的幂是为了后面用按位与替代取模：block_id & (hash_size - 1)。 */
static usize buffer_pool_hash_size(usize num_pages)
{
    usize n = BUFFER_POOL_HASH_MIN;
    while (n < num_pages * 2) n <<= 1;
    return n;
}

/* 计算 block_id 所在 hash bucket。
 * hash_size 必须是 2 的幂，否则按位与不能等价于取模。 */
static usize buffer_pool_hash(BufferPool* pool, block_id_t block_id)
{
    return (usize)block_id & (pool->hash_size - 1);
}

/* 将 BufferPage 指针换算成策略层使用的 buf_id。
 * buf_id 是 pages[] 数组下标，对齐 PG BufferDesc.buf_id 的语义。 */
static usize buffer_pool_buf_id(BufferPool* pool, Page* pg)
{
    assert(pool != NULL);
    assert(pg != NULL);
    assert(pg >= pool->pages);
    assert(pg < pool->pages + pool->num_pages);
    return (usize)(pg - pool->pages);
}

static Page* get_buffer_page(BufferPool* pool, usize buf_id)
{
    assert(pool != NULL);
    assert(buf_id < pool->num_pages);
    return &pool->pages[buf_id];
}

/* 判断指定 buffer slot 是否可以被替换。
 * 只有 valid、未 pinned、且不在装载/写回 I/O 中的 slot 才能作为 victim。 */
static bool buffer_page_evictable(BufferPool* pool, usize buf_id)
{
    Page* pg = get_buffer_page(pool, buf_id);
    return pg->valid && pg->pin_count == 0 &&
           !atomic_load_explicit(&pg->io_in_progress, memory_order_acquire);
}
