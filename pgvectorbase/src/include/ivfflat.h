#ifndef IVFFLAT_H
#define IVFFLAT_H

#include "unvdb.h"
#include "access/genam.h"
#include "nodes/execnodes.h"

/* IVFFlat index options */
typedef struct IvfflatOptions
{
    int32 vl_len_;  /* varlena header (do not touch directly!) */
    int lists;   /* number of lists */
} IvfflatOptions;

IndexBuildResult* ivfflatbuild(Relation heap, Relation index, IndexInfo* indexInfo);
void ivfflatbuildempty(Relation index);
bool ivfflatinsert(Relation indexRelation, Datum* values, bool* isnull, ItemPointer heap_tid,
                   Relation heapRelation, IndexUniqueCheck checkUnique, bool indexUnchanged,
                   struct IndexInfo* indexInfo);
IndexBulkDeleteResult* ivfflatbulkdelete(IndexVacuumInfo* info, IndexBulkDeleteResult* stats,
                                         IndexBulkDeleteCallback callback, void* callback_state);
IndexBulkDeleteResult* ivfflatvacuumcleanup(IndexVacuumInfo* info, IndexBulkDeleteResult* stats);
IndexScanDesc ivfflatbeginscan(Relation index, int nkeys, int norderbys);

void ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
bool ivfflatgettuple(IndexScanDesc scan, ScanDirection dir);

void ivfflatendscan(IndexScanDesc scan);
#endif