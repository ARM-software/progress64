//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#ifndef _EXPECT_H
#define _EXPECT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define EX_HASHSTR(s) #s
#define EX_STR(s) EX_HASHSTR(s)

//Non-fatal error ("warning")
#define EXPECT_W(exp) \
{ \
    if (!(exp)) \
	fprintf(stderr, "FAILURE @ %s:%u; %s\n", __FILE__, __LINE__, EX_STR(exp)); \
}

//Fatal error
#define EXPECT_F(exp) \
{ \
    if (!(exp)) \
	fprintf(stderr, "FAILURE @ %s:%u; %s\n", __FILE__, __LINE__, EX_STR(exp)), abort(); \
}

#ifdef __cplusplus
}
#endif

#endif
