#include "hash.h"
#include "vector.h"

#include <stdio.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg)                \
    do                                  \
    {                                   \
        if (cond)                       \
        {                               \
            pass_count++;               \
            printf("[PASS] %s\n", msg); \
        }                               \
        else                            \
        {                               \
            fail_count++;               \
            printf("[FAIL] %s\n", msg); \
        }                               \
    } while (0)

int main(void)
{
    Vector v;
    CHECK(Vector_init(&v, sizeof(u64), 0) == 0, "Vector_init works from libv");
    u64 x = 42;
    CHECK(vector_push_back(&v, &x) == 0, "vector_push_back works from libv");
    CHECK(*(u64*)vector_get(&v, 0) == 42, "vector_get returns stored value");
    vector_deinit(&v);

    hmap hm;
    hmap_init(&hm, sizeof(u32), sizeof(u64), 8, hmap_int_hash, hmap_int_cmp);
    u32 key = 7;
    u64 val = 99;
    CHECK(hmap_insert(&hm, &key, &val) != NULL, "hmap_insert works from libv");
    hmap_node* node = hmap_get(&hm, &key);
    CHECK(node != NULL && HMAP_VALUE(node, u64) == 99, "hmap_get returns stored value");
    hmap_deinit(&hm);

    printf("\nlibv smoke: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
