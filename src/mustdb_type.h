#ifndef MUSTDB_MUSTDB_TYPE_WRAPPER_H
#define MUSTDB_MUSTDB_TYPE_WRAPPER_H

#if defined(__has_include)
#  if __has_include("libv/mustdb_type.h")
#    include "libv/mustdb_type.h"
#  else
#    include "../libv/src/include/mustdb_type.h"
#  endif
#else
#  include "../libv/src/include/mustdb_type.h"
#endif

#endif /* MUSTDB_MUSTDB_TYPE_WRAPPER_H */
