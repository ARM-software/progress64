//Copyright (c) 2019, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>

#include "p64_errhnd.h"
#include "common.h"
#include "err_hnd.h"
#undef report_error

static THREAD_LOCAL p64_errhnd_cb errh;

p64_errhnd_cb
p64_errhnd_install(p64_errhnd_cb new)
{
    p64_errhnd_cb old = errh;
    errh = new;
    return old;
}

void
report_error(const char *module, const char *error, uintptr_t val)
{
    int action = P64_ERRHND_ABORT;
    if (errh)
    {
	action = errh(module, error, val);
    }
    else
    {
	fprintf(stderr, "Module \"%s\" reported error \"%s\" (%p/%"PRIuPTR")\n",
		module, error, (void *)val, val);
	fflush(NULL);
    }
    switch (action)
    {
	case P64_ERRHND_ABORT :
	    abort();
	case P64_ERRHND_EXIT :
	    exit(EXIT_FAILURE);
	case P64_ERRHND_RETURN :
	    return;
	default :
	    fprintf(stderr, "Error handler returned invalid action %d\n",
		    action);
	    fflush(stderr);
	    abort();
    }
}
