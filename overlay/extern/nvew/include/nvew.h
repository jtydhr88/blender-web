/* SPDX-License-Identifier: MIT
 * NVENC Wrapper — dynamic loading of nvEncodeAPI64.dll, following cuew pattern. */

#pragma once

#include "nvEncodeAPI.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize NVENC: loads nvEncodeAPI64.dll and resolves function pointers.
 * Returns 0 on success, -1 on failure. */
int nvew_init(void);

/* Check if NVENC is available (nvew_init succeeded). */
int nvew_available(void);

/* Get the NVENC function table (filled by NvEncodeAPICreateInstance). */
NV_ENCODE_API_FUNCTION_LIST *nvew_get_functions(void);

/* Get max supported NVENC API version. */
uint32_t nvew_get_max_version(void);

#ifdef __cplusplus
}
#endif
