/* SPDX-License-Identifier: MIT
 * NVENC Wrapper — dynamic loading implementation. */

#include "nvew.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

static int s_initialized = 0;
static int s_available = 0;
static NV_ENCODE_API_FUNCTION_LIST s_funcs;
static uint32_t s_max_version = 0;

typedef NVENCSTATUS(NVENCAPI *PFN_NvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST *functionList);
typedef NVENCSTATUS(NVENCAPI *PFN_NvEncodeAPIGetMaxSupportedVersion)(
    uint32_t *version);

int nvew_init(void)
{
  if (s_initialized) {
    return s_available ? 0 : -1;
  }
  s_initialized = 1;

#ifdef _WIN32
  HMODULE lib = LoadLibraryA("nvEncodeAPI64.dll");
#else
  void *lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
#endif

  if (!lib) {
    return -1;
  }

#ifdef _WIN32
#  define GET_PROC(name) (void *)GetProcAddress(lib, name)
#else
#  define GET_PROC(name) dlsym(lib, name)
#endif

  PFN_NvEncodeAPIGetMaxSupportedVersion getMaxVer =
      (PFN_NvEncodeAPIGetMaxSupportedVersion)GET_PROC(
          "NvEncodeAPIGetMaxSupportedVersion");
  PFN_NvEncodeAPICreateInstance createInstance =
      (PFN_NvEncodeAPICreateInstance)GET_PROC("NvEncodeAPICreateInstance");

  if (!getMaxVer || !createInstance) {
    return -1;
  }

  if (getMaxVer(&s_max_version) != NV_ENC_SUCCESS) {
    return -1;
  }

  memset(&s_funcs, 0, sizeof(s_funcs));
  s_funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;

  if (createInstance(&s_funcs) != NV_ENC_SUCCESS) {
    return -1;
  }

  s_available = 1;
  return 0;

#undef GET_PROC
}

int nvew_available(void)
{
  return s_available;
}

NV_ENCODE_API_FUNCTION_LIST *nvew_get_functions(void)
{
  return s_available ? &s_funcs : NULL;
}

uint32_t nvew_get_max_version(void)
{
  return s_max_version;
}
