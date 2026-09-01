
#include "must.h"

typedef struct ClockSweepPolicy
{
    usize n_buffers; /* buffer slot 总数，对应 PG 的 NBuffers */
    usize clock_hand; /* 时钟指针，对应 PG StrategyControl->nextVictimBuffer 的本地简化版 */
    u8 usage_count_max; /* usage_count 上限，对应 PG BM_MAX_USAGE_COUNT 的可配置版本 */
    u8* usage_counts; /* 每个 buffer slot 的访问热度，语义对应 PG BufferDesc state 中的
                         usage_count*/
} ClockSweepPolicy;

/* 创建 Clock Sweep 策略。
 *
 * 参考 PostgreSQL:
 *   /home/unvdb/cproject/UDB-TX/src/backend/storage/buffer/freelist.c
 *   - ClockSweepTick()
 *   - StrategyGetBuffer()
 *
 * PG 把 nextVictimBuffer、completePasses、numBufferAllocs 等放在共享内存
 * BufferStrategyControl 中；MustDB 是嵌入式单进程 BufferPool，所以只保留
 * n_buffers、clock_hand 和 usage_counts。
 */
static void* create_clock_policy(usize n_buffers, u8 usage_count_max) {}