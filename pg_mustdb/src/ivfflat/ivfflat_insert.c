#include "ivfflat.h"

bool ivfflatinsert(Relation indexRelation, Datum* values, bool* isnull, ItemPointer heap_tid,
                   Relation heapRelation, IndexUniqueCheck checkUnique, bool indexUnchanged,
                   struct IndexInfo* indexInfo)
{
    return false;
}