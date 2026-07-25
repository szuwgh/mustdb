#include "pool.h"

#include <stdlib.h>

/* hash_table 的最小 bucket 数。
 * 实际 bucket 数会扩展到 >= num_pages * 2 的 2 的幂，降低冲突概率。 */
#define BUFFER_POOL_HASH_MIN 16u

/* 根据 BufferPool 容量计算 hash bucket 数。
 * 取 2 的幂是为了后面用按位与替代取模：block_id & (hash_size - 1)。 */
static usize buffer_pool_hash_size(usize num_pages)
{
    usize n = BUFFER_POOL_HASH_MIN;
    while (n < num_pages * 2) n <<= 1;
    return n;
}

static usize buffer_pool_hash(BufferPool* pool, block_id_t block_id)
{
    return (usize)block_id & (pool->hash_size - 1);
}

/* 从全局 LRU 链表中移除一个页面。
 * 典型场景：页面被 pin，或页面即将被 recycle。
 * 双向链表允许 O(1) 删除任意节点。 */
static void buffer_pool_lru_remove(BufferPool* pool, BufferPage* pg)
{
    if (!pg || !pg->in_lru) return;

    /* 修复前驱节点或更新 head。 */
    if (pg->lru_prev)
        pg->lru_prev->lru_next = pg->lru_next;
    else
        pool->lru_head = pg->lru_next;

        /* 修复后继节点或更新 tail。 */
    if (pg->lru_next)
        pg->lru_next->lru_prev = pg->lru_prev;
    else
        pool->lru_tail = pg->lru_prev;

    /* 节点脱链后清空指针，避免后续重复删除或误判。 */
    pg->lru_prev = NULL;
    pg->lru_next = NULL;
    pg->in_lru = false;
}

/* 将未 pin 页面放到 LRU 链表头部。
 * 链表头表示最近刚释放/加入的可淘汰页，链表尾是最久未使用的 victim。 */
static void buffer_pool_lru_push_head(BufferPool* pool, BufferPage* pg)
{
    assert(pg != NULL);
    assert(pg->pin_count == 0);
       /* 如果已经在 LRU 中，先摘下再插到头部，保持链表唯一性。 */
    if (pg->in_lru) buffer_pool_lru_remove(pool, pg);

    pg->lru_prev = NULL;
    pg->lru_next = pool->lru_head;
    if (pool->lru_head)
        pool->lru_head->lru_prev = pg;
    else
        pool->lru_tail = pg;
    pool->lru_head = pg;
    pg->in_lru = true;
}

/* 将页面加入 dirty list。
 * 调用方必须已持有 pool_lock。 */
static void buffer_pool_dirty_add(BufferPool* pool, BufferPage* pg)
{
    assert(pg != NULL);
    if (pg->in_dirty) return;

    pg->dirty_prev = NULL;
    pg->dirty_next = pool->dirty_head;
    if (pool->dirty_head)
        pool->dirty_head->dirty_prev = pg;
    else
        pool->dirty_tail = pg;
    pool->dirty_head = pg;
    pg->in_dirty = true;
    pool->dirty_count++;
}

/* 创建固定容量 BufferPool。
 * pages 数组一次性分配；Block/FileBuffer 在页面首次装入时再创建。*/
BufferPool* buffer_pool_create(usize num_pages)
{
    assert(num_pages > 0);
    BufferPool* pool = (BufferPool*)malloc(sizeof(BufferPool));
}