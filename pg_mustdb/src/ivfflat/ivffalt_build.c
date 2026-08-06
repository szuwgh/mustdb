#include "ivfflat.h"

IndexBuildResult* ivfflatbuild(Relation heap, Relation index, IndexInfo* indexInfo)
{
    IndexBuildResult* result;
    result = (IndexBuildResult*)palloc(sizeof(IndexBuildResult));
    table_index_build_scan(heapRelation, indexRelation, indexInfo, false, true,
                           mustdb_build_callback, (void*)&buildstate, NULL);
    return result;
}

void ivfflatbuildempty(Relation index)
{
    void;
}
