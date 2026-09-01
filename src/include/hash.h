#ifndef MUSTDB_HASH_WRAPPER_H
#define MUSTDB_HASH_WRAPPER_H

#if defined(__has_include)
#if __has_include("libv/hash.h")
#include "libv/hash.h"
#else
#include "../libv/src/include/hash.h"
#endif
#else
#include "../libv/src/include/hash.h"
#endif

#endif /* MUSTDB_HASH_WRAPPER_H */
