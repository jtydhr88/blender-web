set(INC
  PUBLIC include
)

set(SRC
  src/nvew.c
  include/nvew.h
  include/nvEncodeAPI.h
)

set(LIB)

blender_add_lib(extern_nvew "${SRC}" "${INC}" "" "${LIB}")
