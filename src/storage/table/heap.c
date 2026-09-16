#include "heap.h"

ItemPtr heap_insert(HeapStore* heap, TxnId xid, u32 cid, ItemPtr emb_ctid, const Datum* values,
                    u64 null_bits)
{
    /* Implementation for heap insert */
    return (ItemPtr){0};
}

int heap_create(HeapStore* store, const HeapStoreContext* ctx)
{
    /* Implementation for heap create */
    return 0;
}

int heap_open(HeapStore* store, const HeapStoreContext* ctx)
{
    /* Implementation for heap open */
    return 0;
}