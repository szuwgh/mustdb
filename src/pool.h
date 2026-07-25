#ifndef POOL_H
#define POOL_H

#include "vb_type.h"
#include "storage.h"
#include "assert.h"
#include "lock.h"

/* 默认 BufferPool frame 数。
 * 每个 frame 缓存一个 Block/FileBuffer 页面。 */
#define BUFFER_POOL_DEFAULT_PAGES 64
typedef struct BufferPage BufferPage;
typedef struct BufferPool BufferPool;

struct BufferPage
{
    Block* block; /* 缓存的物理块对象；block->id 是块号，block->fb->buffer 是可读写页面数据区 */
    int pin_count; /* 当前被 pin 的次数；大于 0 表示仍有调用者持有页面，不能被淘汰 */
    bool dirty; /* 页面是否被修改；dirty 页面在淘汰或 flush 时必须写回 BlockManager */
    bool valid; /* slot 是否已被占用；false 表示该 BufferPage 为空闲槽，可直接装入新 Block */
    BufferPage* hash_next;/* block_id 哈希冲突链；用于同一个 hash bucket 内链接多个 BufferPage */
    BufferPage* lru_prev; /* 未 pin 页面 LRU 双向链表前驱；pin_count==0 且 valid 时才会挂链 */
    BufferPage* lru_next;   /* 未 pin 页面 LRU 双向链表后继；链表头是最近释放，链表尾是淘汰候选 */
    BufferPage* dirty_prev; /* dirty list 前驱；dirty=true 时才会挂入 BufferPool.dirty_head 链 */
    BufferPage* dirty_next; /* dirty list 后继；flush/recycle 写回后会从 dirty list 摘除 */
    BufferPage* free_next; /* 空 frame 链；valid=false 时挂在 BufferPool.free_head 上 */
    LWLock content_lock; /* 页面内容读写锁；pin_count 只管生命周期，content_lock 管 page bytes */
    bool in_lru; /* 当前是否挂在未 pin LRU 链表中；避免重复插入或错误删除 */
    bool in_dirty; /* 当前是否挂在 dirty list 中；避免同一 dirty page 重复入链 */
};

struct BufferPool
{
    BufferPage* pages;
    usize num_pages;
    BufferPage** hash_table;
    usize hash_size;
    BufferPage* lru_head; /* 最近 unpin/add 的可淘汰页面 */
    BufferPage* lru_tail; /* 最久未使用的可淘汰页面，优先作为 victim */
    BufferPage* dirty_head; /* dirty 页面链表头；flush 从这里遍历，不再扫描所有 frame */
    BufferPage* dirty_tail; /* dirty 页面链表尾；用于 O(1) 摘除/维护链表 */
    BufferPage* free_head; /* 尚未使用的空 frame 链表头；pop 后即可装入新 Block */
    usize dirty_count; /* dirty list 中页面数量 */
    slock_t pool_lock; /* BufferPool 元数据锁：hash/LRU/free/dirty/pin_count */
};

/* 创建固定容量 BufferPool。
 * num_pages 表示最多同时缓存多少个页面 frame。 */
BufferPool* buffer_pool_create(usize num_pages);

/* 销毁 BufferPool。
 * 调用前所有页面必须已经 unpin，否则内部 assert 会失败。 */
void buffer_pool_destroy(BufferPool* pool);

/* Pin 一个页面并返回可读写 page buffer。
 *
 * 命中缓存时增加 pin_count 并返回 Block->fb->buffer。
 * 未命中且 bm != NULL 时，会通过 BlockManager 读取页面。
 * 未命中且 bm == NULL 时，表示 standalone 模式无法加载缺失页，返回 NULL。
 *
 * 调用方使用完成后必须调用 buffer_pool_unpin。 */
u8* buffer_pool_pin(BufferPool* pool, block_id_t block_id, void* bm);

/* WAL-aware pin：miss 淘汰 dirty victim 前会先 flush WAL 到 victim.page_lsn。 */
u8* buffer_pool_pin_wal(BufferPool* pool, block_id_t block_id, void* bm, void* wal);

/* Unpin 一个页面。
 *
 * pin_count 减 1；当 pin_count 变为 0 时，该页面重新进入 LRU 可淘汰队列。
 * 如果页面 dirty，后续淘汰或 flush 时会写回 BlockManager。 */
void buffer_pool_unpin(BufferPool* pool, block_id_t block_id);

/* 标记页面已被修改。
 *
 * dirty 页面在淘汰或 flush 前需要写回 BlockManager。
 * 当前接口只记录 dirty 位；完整 WAL/page LSN 语义由上层保证。 */
void buffer_pool_mark_dirty(BufferPool* pool, block_id_t block_id);

/* 标记页面 dirty 并记录该页面对应的最高 WAL end-LSN。 */
void buffer_pool_mark_dirty_lsn(BufferPool* pool, block_id_t block_id, u64 page_lsn);

/* 对已 pin 的页面加内容锁。
 *
 * pin_count 只保护 frame 生命周期；content lock 保护 page bytes 的并发读写。
 * 调用顺序必须是：pin -> lock_page -> read/write -> unlock_page -> unpin。 */
int buffer_pool_lock_page(BufferPool* pool, block_id_t block_id, LWLockMode mode);

/* 释放页面内容锁。 */
int buffer_pool_unlock_page(BufferPool* pool, block_id_t block_id);

/* 当前 dirty list 中的页面数量，主要用于测试和诊断。 */
usize buffer_pool_dirty_count(BufferPool* pool);

/* 查询指定页面是否在 dirty 状态，主要用于测试和诊断。 */
bool buffer_pool_is_dirty(BufferPool* pool, block_id_t block_id);

/* 主动刷出所有未 pinned 的 dirty 页面。
 *
 * 通过 bm 写回 BlockManager。
 * 返回实际刷出的页面数量。
 * bm == NULL 时不会写回，返回 0。 */
usize buffer_pool_flush_all(BufferPool* pool, void* bm);

/* WAL-aware flush：写 dirty page 前先 wal_flush_upto(page_lsn)。 */
usize buffer_pool_flush_all_wal(BufferPool* pool, void* bm, void* wal);

/* 查询指定页面当前 pin_count，主要用于测试和调试。 */
int buffer_pool_pin_count(BufferPool* pool, block_id_t block_id);

/* 将一个外部已初始化页面导入 BufferPool。
 *
 * page_data 属于调用方或临时 Block，BufferPool 不能接管其生命周期，
 * 因此会复制到 BufferPool 自己的 cache-owned buffer。
 * 返回 BufferPool 内部 buffer 指针。 */
u8* buffer_pool_add_page(BufferPool* pool, block_id_t block_id, const u8* page_data);

/* 创建或替换一个 cache-owned 页面，并返回可直接写入的 buffer。
 *
 * 适用于调用方先从 BufferPool 获取内部 page buffer，
 * 然后直接在该 buffer 上初始化页面的路径。
 * 相比 buffer_pool_add_page，可避免外部 page_data -> BufferPool 的额外 memcpy。 */
u8* buffer_pool_new_page(BufferPool* pool, block_id_t block_id);

/* 创建或替换一个 pinned 的 cache-owned 新页面。
 *
 * 调用方直接在返回 buffer 上构造页面；完成后必须 buffer_pool_unpin。
 * 该接口不主动标 dirty，调用方应在 WAL finish 后用
 * buffer_pool_mark_dirty_lsn 记录 dirty/page_lsn。 */
u8* buffer_pool_new_page_pinned_wal(BufferPool* pool, block_id_t block_id, void* bm, void* wal);
#endif