#ifndef MUSTDB_VECTOR_WRAPPER_H
#define MUSTDB_VECTOR_WRAPPER_H

#if defined(__has_include)
#  if __has_include("libv/vector.h")
#    include "libv/vector.h"
#  else
#    include "../libv/src/include/vector.h"
#  endif
#else
#  include "../libv/src/include/vector.h"
#endif

#endif /* MUSTDB_VECTOR_WRAPPER_H */
