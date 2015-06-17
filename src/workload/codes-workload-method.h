/*
 * Copyright (C) 2013 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* I/O workload generator API to be used by workload generator methods.  
 * It mimics the top level codes-workload.h API, except that there is no 
 * reverse handler.
 */

#ifndef CODES_WORKLOAD_METHOD_H
#define CODES_WORKLOAD_METHOD_H

#include "ross.h"
#include "codes/codes-workload.h"

#define APP_ID_UNSUPPORTED(id, name) \
    if (id != 0) \
        tw_error(TW_LOC,\
                "APP IDs not supported for %s generator, 0 required", name);



struct codes_workload_method
{
    char *method_name; /* name of the generator */
    int (*codes_workload_load)(const char* params, int app_id, int rank);
    void (*codes_workload_get_next)(int app_id, int rank, struct codes_workload_op *op);
    int (*codes_workload_get_rank_cnt)(const char* params, int app_id);
};

#endif /* CODES_WORKLOAD_METHOD_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
