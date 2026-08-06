#include "ivfflat.h"

IndexScanDesc ivfflatbeginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    scan = RelationGetIndexScan(index, nkeys, norderbys);
    return scan;
}

void ivfflatrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    void;
}

void ivfflatendscan(IndexScanDesc scan)
{
    void;
}

bool ivfflatgettuple(IndexScanDesc scan, ScanDirection dir) {}