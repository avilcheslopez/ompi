/*
 *
 * Copyright (c) 2009-2012 Mellanox Technologies.  All rights reserved.
 * Copyright (c) 2009-2012 Oak Ridge National Laboratory.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */
#include <stdarg.h>

#include "opal/mca/base/mca_base_param.h"
#include "ompi/include/ompi/constants.h"
#include "netpatterns.h"

int netpatterns_base_verbose = 0; /* disabled by default */

int netpatterns_register_mca_params(void)
{
    mca_base_param_reg_int_name("common", 
                                "netpatterns_base_verbose", 
                                "Verbosity level of the NETPATTERNS framework", 
                                false, false, 
                                0, 
                                &netpatterns_base_verbose);

    return OMPI_SUCCESS;
}

int netpatterns_base_err(const char* fmt, ...)
{
    va_list list;
    int ret;

    va_start(list, fmt);
    ret = vfprintf(stderr, fmt, list);
    va_end(list);
    return ret;
}

int netpatterns_init(void)
{
/* There is no component for common_netpatterns so every component that uses it
   should call netpatterns_init, still we want to run it only once */
static int was_called = 0;

    if (0 == was_called) {
        was_called = 1;
    
        return netpatterns_register_mca_params();
    }

    return OMPI_SUCCESS;
}