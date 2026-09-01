#include "alloc.h"

#include <stdio.h>
#include <string.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (cond) { pass_count++; printf("[PASS] %s\n", (msg)); } \
        else      { fail_count++; printf("[FAIL] %s\n", (msg)); } \
    } while (0)

static void check_stats_eq(MAllocStatsSnap st, u64 allocs, u64 frees, u64 reallocs,
                           u64 live, const char* msg)
{
    CHECK(st.alloc_count == allocs && st.free_count == frees &&
              st.realloc_count == reallocs && st.bytes_live == live,
          msg);
}

static void test_malloc_calloc_free_stats(void)
{
    printf("\n=== mmalloc/mcalloc/mfree stats ===\n");

    MAllocStatsSnap st = get_mem_stats();
    check_stats_eq(st, 0, 0, 0, 0, "allocator stats start empty in a fresh process");

    u8* p = (u8*)mmalloc(16);
    CHECK(p != NULL, "mmalloc allocates user memory");
    memset(p, 0x5a, 16);
    st = get_mem_stats();
    check_stats_eq(st, 1, 0, 0, 16, "mmalloc tracks alloc count and live bytes");
    CHECK(st.bytes_peak >= 16, "mmalloc updates peak live bytes");

    i32* zeroed = (i32*)mcalloc(4, sizeof(i32));
    CHECK(zeroed != NULL, "mcalloc allocates memory");
    CHECK(zeroed[0] == 0 && zeroed[1] == 0 && zeroed[2] == 0 && zeroed[3] == 0,
          "mcalloc zeroes the user buffer");
    st = get_mem_stats();
    check_stats_eq(st, 2, 0, 0, 32, "mcalloc tracks allocation stats");

    mfree(zeroed);
    st = get_mem_stats();
    check_stats_eq(st, 2, 1, 0, 16, "mfree decrements live bytes");

    mfree(p);
    st = get_mem_stats();
    check_stats_eq(st, 2, 2, 0, 0, "mfree releases all tracked live bytes");

    mfree(NULL);
    st = get_mem_stats();
    check_stats_eq(st, 2, 2, 0, 0, "mfree(NULL) is a no-op");
}

static void test_realloc_semantics_and_stats(void)
{
    printf("\n=== mrealloc semantics and stats ===\n");

    MAllocStatsSnap base = get_mem_stats();

    u8* p = (u8*)mrealloc(NULL, 8);
    CHECK(p != NULL, "mrealloc(NULL, size) behaves like mmalloc");
    memset(p, 0x31, 8);

    MAllocStatsSnap st = get_mem_stats();
    CHECK(st.alloc_count == base.alloc_count + 1 && st.realloc_count == base.realloc_count &&
              st.bytes_live == base.bytes_live + 8,
          "mrealloc(NULL, size) updates allocation stats like malloc");

    u8* grown = (u8*)mrealloc(p, 24);
    CHECK(grown != NULL, "mrealloc grows an allocation");
    CHECK(grown[0] == 0x31 && grown[7] == 0x31, "mrealloc grow preserves old bytes");
    st = get_mem_stats();
    CHECK(st.realloc_count == base.realloc_count + 1 && st.bytes_live == base.bytes_live + 24,
          "mrealloc grow updates realloc count and live bytes");
    CHECK(st.bytes_peak >= base.bytes_live + 24, "mrealloc grow updates peak live bytes");

    u8* shrunk = (u8*)mrealloc(grown, 4);
    CHECK(shrunk != NULL, "mrealloc shrinks an allocation");
    CHECK(shrunk[0] == 0x31 && shrunk[3] == 0x31, "mrealloc shrink preserves prefix bytes");
    st = get_mem_stats();
    CHECK(st.realloc_count == base.realloc_count + 2 && st.bytes_live == base.bytes_live + 4,
          "mrealloc shrink updates realloc count and live bytes");

    void* null_after_free = mrealloc(shrunk, 0);
    CHECK(null_after_free == NULL, "mrealloc(ptr, 0) frees and returns NULL");
    st = get_mem_stats();
    CHECK(st.free_count == base.free_count + 1 && st.bytes_live == base.bytes_live,
          "mrealloc(ptr, 0) records a free and clears live bytes");
}

static void test_allocation_failures_do_not_change_stats(void)
{
    printf("\n=== allocation failure accounting ===\n");

    MAllocStatsSnap before = get_mem_stats();

    void* too_large = mmalloc((usize)-1);
    CHECK(too_large == NULL, "mmalloc rejects size overflow");
    MAllocStatsSnap after = get_mem_stats();
    CHECK(after.alloc_count == before.alloc_count && after.free_count == before.free_count &&
              after.realloc_count == before.realloc_count && after.bytes_live == before.bytes_live,
          "failed mmalloc does not change stats");

    void* calloc_overflow = mcalloc(2, ((usize)-1 / 2) + 1);
    CHECK(calloc_overflow == NULL, "mcalloc rejects multiplication overflow");
    after = get_mem_stats();
    CHECK(after.alloc_count == before.alloc_count && after.bytes_live == before.bytes_live,
          "failed mcalloc does not change stats");

    u8* p = (u8*)mmalloc(8);
    CHECK(p != NULL, "mmalloc succeeds before failed realloc");
    if (!p) return;
    memset(p, 0x7c, 8);

    MAllocStatsSnap with_live = get_mem_stats();
    void* failed_realloc = mrealloc(p, (usize)-1);
    CHECK(failed_realloc == NULL, "mrealloc rejects size overflow");
    CHECK(p[0] == 0x7c && p[7] == 0x7c, "failed mrealloc leaves old allocation valid");
    after = get_mem_stats();
    CHECK(after.alloc_count == with_live.alloc_count && after.free_count == with_live.free_count &&
              after.realloc_count == with_live.realloc_count &&
              after.bytes_live == with_live.bytes_live,
          "failed mrealloc does not change stats");

    mfree(p);
    after = get_mem_stats();
    CHECK(after.bytes_live == before.bytes_live, "free after failed realloc restores live bytes");
}

static void test_fault_injection_when_enabled(void)
{
#if MUSTDB_ENABLE_ALLOC_FAULTS
    printf("\n=== allocation fault injection ===\n");

    MAllocStatsSnap base = get_mem_stats();
    set_mem_fault_countdown(0);
    CHECK(mmalloc(8) == NULL, "fault countdown 0 fails mmalloc");
    CHECK(mcalloc(2, sizeof(i32)) == NULL, "fault countdown 0 fails mcalloc through mmalloc");
    MAllocStatsSnap after_fail = get_mem_stats();
    CHECK(after_fail.alloc_count == base.alloc_count && after_fail.free_count == base.free_count &&
              after_fail.realloc_count == base.realloc_count &&
              after_fail.bytes_live == base.bytes_live,
          "faulted mmalloc/mcalloc do not change stats");

    clear_mem_fault();
    u8* p = (u8*)mmalloc(8);
    CHECK(p != NULL, "clear_mem_fault restores mmalloc");
    if (!p) return;
    memset(p, 0x5a, 8);

    MAllocStatsSnap before_realloc = get_mem_stats();
    set_mem_fault_countdown(0);
    void* failed_realloc = mrealloc(p, 16);
    CHECK(failed_realloc == NULL, "fault countdown 0 fails mrealloc");
    CHECK(p[0] == 0x5a && p[7] == 0x5a, "faulted mrealloc keeps old allocation valid");
    MAllocStatsSnap after_realloc = get_mem_stats();
    CHECK(after_realloc.alloc_count == before_realloc.alloc_count &&
              after_realloc.free_count == before_realloc.free_count &&
              after_realloc.realloc_count == before_realloc.realloc_count &&
              after_realloc.bytes_live == before_realloc.bytes_live,
          "faulted mrealloc does not change stats");

    clear_mem_fault();
    u8* grown = (u8*)mrealloc(p, 16);
    CHECK(grown != NULL, "clear_mem_fault restores mrealloc");
    if (grown) p = grown;
    CHECK(p[0] == 0x5a && p[7] == 0x5a, "successful mrealloc after clear preserves bytes");
    mfree(p);

    set_mem_fault_countdown(1);
    void* first = mmalloc(4);
    void* second = mmalloc(4);
    CHECK(first != NULL, "fault countdown 1 lets first allocation pass");
    CHECK(second == NULL, "fault countdown 1 fails second allocation");
    mfree(first);
    clear_mem_fault();

    MAllocStatsSnap done = get_mem_stats();
    CHECK(done.bytes_live == base.bytes_live, "fault injection test restores live bytes");
#else
    (void)set_mem_fault_countdown;
    (void)clear_mem_fault;
#endif
}

int main(void)
{
    test_malloc_calloc_free_stats();
    test_realloc_semantics_and_stats();
    test_allocation_failures_do_not_change_stats();
    test_fault_injection_when_enabled();

    printf("\nAlloc tests: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
