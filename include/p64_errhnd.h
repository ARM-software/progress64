//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

//Error handler API for Progress64

#ifndef _P64_ERRHND_H
#define _P64_ERRHND_H

#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

//Return codes for error handler
#define P64_ERRHND_ABORT  0  //Call abort(), this may generate a core dump
#define P64_ERRHND_EXIT   1  //Call exit(EXIT_FAILURE)
#define P64_ERRHND_RETURN 2  //Return to caller, call fails or is ignored

//User defined error handler
typedef int (*p64_errhnd_cb)(const char *module,
			     const char *error,
			     uintptr_t val);

//Install user-defined error handler
//Specify NULL to uninstall
//Return previous installed error handler
p64_errhnd_cb p64_errhnd_install(p64_errhnd_cb errh);

#ifdef __cplusplus
}
#endif

#endif
