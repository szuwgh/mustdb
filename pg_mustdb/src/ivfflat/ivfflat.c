#include "unvdb.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "access/amapi.h"
#include "ivfflat.h"
#include "nodes/pathnodes.h"

static void ivfflatcostestimate(PlannerInfo* root, IndexPath* path, double loop_count,
                                Cost* indexStartupCost, Cost* indexTotalCost,
                                Selectivity* indexSelectivity, double* indexCorrelation,
                                double* indexPages)
{
}

static bytea* ivfflatoptions(Datum reloptions, bool validate)
{
    if (validate && PointerIsValid(DatumGetPointer(reloptions)))
        ereport(ERROR, (errmsg("mustdb_ivf index options are not supported yet")));

    return NULL;
}

static bool ivfflatvalidate(Oid opclassoid)
{
    return true;
}

PG_FUNCTION_INFO_V1(mustdb_ivfflathandler);
Datum mustdb_ivfflathandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine* amroutine = makeNode(IndexAmRoutine);
    amroutine->amstrategies = 0;
    amroutine->amsupport = 5;
    amroutine->amoptsprocnum = 0;
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = true;
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = false;
    amroutine->amoptionalkey = true;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing = false;
    amroutine->amparallelvacuumoptions = 0;
    amroutine->amkeytype = InvalidOid;

    amroutine->ambuild = ivfflatbuild;
    amroutine->ambuildempty = ivfflatbuildempty;
    amroutine->aminsert = ivfflatinsert;
    amroutine->ambulkdelete = ivfflatbulkdelete;
    amroutine->amvacuumcleanup = ivfflatvacuumcleanup;
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = ivfflatcostestimate;
    amroutine->amoptions = ivfflatoptions;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = ivfflatvalidate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = ivfflatbeginscan;
    amroutine->amrescan = ivfflatrescan;
    amroutine->amgettuple = ivfflatgettuple;
    amroutine->amgetbitmap = NULL;
    amroutine->amendscan = ivfflatendscan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;

    PG_RETURN_POINTER(amroutine);
}