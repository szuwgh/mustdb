#include "ivfflat.h"

IndexBulkDeleteResult* ivfflatbulkdelete(IndexVacuumInfo* info, IndexBulkDeleteResult* stats,
                                         IndexBulkDeleteCallback callback, void* callback_state)
{
    if (stats == NULL) stats = (IndexBulkDeleteResult*)palloc0(sizeof(IndexBulkDeleteResult));

    return stats;
}