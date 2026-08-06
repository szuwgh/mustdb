#include "ivfflat.h"
IndexBulkDeleteResult* ivfflatvacuumcleanup(IndexVacuumInfo* info, IndexBulkDeleteResult* stats)
{
    (void)info;

    if (stats == NULL) stats = (IndexBulkDeleteResult*)palloc0(sizeof(IndexBulkDeleteResult));
    return stats;
}