#ifndef VECTORBASE_VB_TYPE_WRAPPER_H
#define VECTORBASE_VB_TYPE_WRAPPER_H

#if defined(__has_include)
#  if __has_include("libv/vb_type.h")
#    include "libv/vb_type.h"
#  else
#    include "../libv/src/include/vb_type.h"
#  endif
#else
#  include "../libv/src/include/vb_type.h"
#endif

#endif /* VECTORBASE_VB_TYPE_WRAPPER_H */
