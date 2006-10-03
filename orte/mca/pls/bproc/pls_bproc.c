/* -*- C -*-
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
/**
 * @file:
 * Part of the bproc launcher. See pls_bproc.h for an overview of how it works.
 */

#include "orte_config.h"
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif  /* HAVE_SYS_STAT_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <errno.h>
#include <signal.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif  /* HAVE_FCNTL_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */

#include "opal/install_dirs.h"
#include "opal/event/event.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/util/os_path.h"
#include "opal/util/show_help.h"
#include "opal/util/trace.h"

#include "orte/dss/dss.h"
#include "orte/util/sys_info.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/iof/iof.h"
#include "orte/mca/gpr/gpr.h"
#include "orte/mca/ns/ns.h"
#include "orte/mca/sds/base/base.h"
#include "orte/mca/oob/base/base.h"
#include "orte/mca/ras/base/base.h"
#include "orte/mca/rmgr/rmgr.h"
#include "orte/mca/rmaps/rmaps_types.h"
#include "orte/mca/rml/rml.h"
#include "orte/mca/smr/smr.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/runtime.h"

/* remove this when moved to 2.0 */
#include "orte/mca/rmaps/base/rmaps_private.h"

#include "orte/mca/pls/base/pls_private.h"
#include "pls_bproc.h"

/**
 * Our current evironment
 */
#if !defined(__WINDOWS__)
extern char **environ;
#endif  /* !defined(__WINDOWS__) */

#if OMPI_HAVE_POSIX_THREADS && OMPI_THREADS_HAVE_DIFFERENT_PIDS
int orte_pls_bproc_launch_threaded(orte_jobid_t);
#endif


/**
 * Initialization of the bproc module with all the needed function pointers
 */
orte_pls_base_module_t orte_pls_bproc_module = {
#if OMPI_HAVE_POSIX_THREADS && OMPI_THREADS_HAVE_DIFFERENT_PIDS
    orte_pls_bproc_launch_threaded,
#else
    orte_pls_bproc_launch,
#endif
    orte_pls_bproc_terminate_job,
    orte_pls_bproc_terminate_orteds,
    orte_pls_bproc_terminate_proc,
    orte_pls_bproc_signal_job,
    orte_pls_bproc_signal_proc,
    orte_pls_bproc_finalize
};


static int orte_pls_bproc_node_array(orte_rmaps_base_map_t* map,
                                     int ** node_array, int * node_array_len);
static int orte_pls_bproc_node_list(int * node_array, int node_array_len,
                                    int ** node_list, int * num_nodes,
                                    int num_procs);
static int orte_pls_bproc_setup_io(orte_jobid_t jobid, struct bproc_io_t * io,
                                   int node_rank, int app_context);
static void orte_pls_bproc_waitpid_cb(pid_t wpid, int status, void *data);
static void orte_pls_bproc_waitpid_daemon_cb(pid_t wpid, int status, void *data);
#ifdef MCA_pls_bproc_scyld
/* compatibility functions for scyld bproc and pre 3.2.0 LANL bproc */
static int bproc_vexecmove_io(int nnodes, int *nodes, int *pids,
                              struct bproc_io_t *io, int iolen, const char *cmd,
                              char * const argv[], char * envp[]);
static int bproc_vexecmove(int nnodes, int *nodes, int *pids, const char *cmd,
                           char * const argv[], char * envp[]);
#endif
static void orte_pls_bproc_setup_env(char *** env);
static int orte_pls_bproc_launch_daemons(orte_cellid_t cellid, char *** envp,
                                         int ** node_arrays, int * node_array_lens,
                                         int num_contexts, int num_procs,
                                         orte_vpid_t global_vpid_start,
                                         orte_jobid_t jobid, int* num_daemons);
static int orte_pls_bproc_launch_app(orte_cellid_t cellid, orte_jobid_t jobid,
                                     orte_rmaps_base_map_t* map, int num_processes,
                                     orte_vpid_t vpid_start,
                                     orte_vpid_t global_vpid_start,
                                     int app_context,
                                     int * node_array, int node_array_len);

/**
 * creates an array that is indexed by the node number and each entry contains the
 * number of processes that will be launched on that node.
 *
 * @param map single context mapping
 * @param node_array a pointer to put the node array into
 * @param node_array_len returns the length of the array
 * @retval >=0 the number of processes
 * @retval <0 orte err
 */
static int orte_pls_bproc_node_array(orte_rmaps_base_map_t* map,
                                     int ** node_array, int * node_array_len) {
    opal_list_item_t* item;
    int num_procs = 0;
    int num_on_node;

    OPAL_TRACE(1);
    
    *node_array_len = 0;
    for(item =  opal_list_get_first(&map->nodes);
        item != opal_list_get_end(&map->nodes);
        item =  opal_list_get_next(item)) {
        if(*node_array_len < atol(((orte_rmaps_base_node_t*)item)->node->node_name)) {
            *node_array_len = atol(((orte_rmaps_base_node_t*)item)->node->node_name);
        }
    }
    (*node_array_len)++;
    /* build the node array */
    *node_array = (int*)malloc(sizeof(int) * *node_array_len);
    if(NULL == *node_array) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }
    memset(*node_array, 0, sizeof(int) * *node_array_len);

    for(item =  opal_list_get_first(&map->nodes);
        item != opal_list_get_end(&map->nodes);
        item =  opal_list_get_next(item)) {
        orte_rmaps_base_node_t* node = (orte_rmaps_base_node_t*)item;
        num_on_node = opal_list_get_size(&node->node_procs);
        (*node_array)[atol(node->node->node_name)] += num_on_node;
        num_procs += num_on_node;
    }
    return num_procs;
}

/**
 * Creates a bproc nodelist from a node array.
 * @param node_array an array of bproc nodes that contains the number of processes
 *                   to be launched on each node
 * @param node_array_len the length of the node array
 * @param node_list a pointer that the bproc node list will be returned in
 * @param num_nodes a pointer to return the number of nodes in the node list
 * @param num_procs the number of processes that a node must have to be on the
 *                  node list
 */
static int orte_pls_bproc_node_list(int * node_array, int node_array_len,
                                   int ** node_list, int * num_nodes,
                                   int num_procs) {
    int node;

    OPAL_TRACE(1);
    
    *num_nodes = 0;
    *node_list = (int*)malloc(sizeof(int) * node_array_len);
    if(NULL == *node_list) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }

    /* build the node list */
    for(node = 0; node < node_array_len; node++) {
        if(node_array[node] >= num_procs) {
            (*node_list)[(*num_nodes)++] = node;
        }
    }
    return ORTE_SUCCESS;
}

/**
 * Sets up the bproc io structs for the specified rank on the nodes
 *
 * @param jobid
 * @param io A pointer to an array of 3 bproc_io_t structs
 * @param node_rank the rank on the node we are setting up the structs for
 * @param app_context the application context number
 * @retval ORTE_SUCCESS
 * @retval error
 */
static int orte_pls_bproc_setup_io(orte_jobid_t jobid, struct bproc_io_t * io,
                                   int node_rank, int app_context) {
    char *frontend = NULL, *path = NULL, *job = NULL;
    int rc, i;

    OPAL_TRACE(1);
    
    /* ensure that system info is set */
    orte_sys_info();
    if (NULL == orte_system_info.user) { /* error condition */
        return ORTE_ERROR;
    }
    if (NULL == orte_universe_info.name) {  /* error condition */
        return ORTE_ERROR;
    }

    rc = orte_ns.convert_jobid_to_string(&job, jobid);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    /* build the directory tree the io files will be in */
    if (0 > asprintf(&frontend, OPAL_PATH_SEP"tmp"OPAL_PATH_SEP"openmpi-bproc-%s"OPAL_PATH_SEP"%s"OPAL_PATH_SEP"%s-%d"OPAL_PATH_SEP"%d",
                     orte_system_info.user, orte_universe_info.name, job,
                     app_context, node_rank)) {
        rc = ORTE_ERR_OUT_OF_RESOURCE;
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }

    for(i = 0; i < 3; i++)  {
        if(0 > asprintf(&path, "%s"OPAL_PATH_SEP"%d", frontend, i)) {
            rc = ORTE_ERR_OUT_OF_RESOURCE;
            ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
            goto cleanup;
        }
        if (mca_pls_bproc_component.debug) {
            opal_output(0, "mpirun bproc io setup. Path: %s\n", path);
        }
        io[i].fd = i;
        io[i].type = BPROC_IO_FILE;
#if defined BPROC_API_VERSION && BPROC_API_VERSION >= 4
        io[i].flags = 0;
#else
        io[i].send_info = 0;
#endif
        if(0 == i) {
            io[i].d.file.flags = O_RDONLY;
        } else {
            io[i].d.file.flags = O_WRONLY;
        }
        io[i].d.file.offset = 0;
        io[i].d.file.mode = 0;
        strncpy(io[i].d.file.name, path, 256);
        free(path);
    }

 cleanup:
    if (NULL != frontend) {
       free(frontend);
    }
    if (NULL != job) {
        free(job);
    }
    return rc;
}

/**
 * Callback for orte_wait_cb. This function ONLY gets called for
 * normal termination, or termination caused by a signal. If the
 * process abnormally terminates by other than a signal, we go through
 * another function so it can tell us that it was abnormal.
 * Bproc doesn't really let us do it through here.
 * @param wpid the process's pid
 * @param status tells why the process died
 * @param data a pointer to the process's name
 */
static void orte_pls_bproc_waitpid_cb(pid_t wpid, int status, void *data) {
    orte_process_name_t * proc = (orte_process_name_t*) data;
    int rc;
    
    OPAL_TRACE(1);
    
    /* set the state of this process */
    if(WIFEXITED(status)) {
            rc = orte_smr.set_proc_state(proc, ORTE_PROC_STATE_TERMINATED, status);
    } else {
        rc = orte_smr.set_proc_state(proc, ORTE_PROC_STATE_ABORTED, status);
    }
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
    }
    free(proc);
}

/**
 * Callback for orte_wait_cb for the daemons. If a daemon unexpectedly dies
 * before we are done launching, we abort the job.
 * @param wpid the daemons's pid
 * @param status tells why the daemon died
 * @param data a pointer to the node the daemon was on
 */
static void orte_pls_bproc_waitpid_daemon_cb(pid_t wpid, int status, void *data) {

    OPAL_TRACE(1);
     
    if(!mca_pls_bproc_component.done_launching) {
        /* if a daemon exits before we are done launching the user apps we send a
         * message to ourself so we will break out of the receive loop and exit */
        orte_buffer_t ack;
        int rc;
        int src[4] = {-1, -1};
        src[2] = wpid;
        src[3] = *(int*)data;
        if(WIFSIGNALED(status)) {
            src[1] = WTERMSIG(status);
        }
        OBJ_CONSTRUCT(&ack, orte_buffer_t);
        rc = orte_dss.pack(&ack, &src, 4, ORTE_INT);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
        }
        rc = mca_oob_send_packed(ORTE_RML_NAME_SELF, &ack, ORTE_RML_TAG_BPROC, 0);
        if(0 > rc) {
            ORTE_ERROR_LOG(rc);
        }
    }
    OPAL_THREAD_LOCK(&mca_pls_bproc_component.lock);
    if(0 < mca_pls_bproc_component.num_daemons) {
        mca_pls_bproc_component.num_daemons--;
    }
    opal_condition_signal(&mca_pls_bproc_component.condition);
    OPAL_THREAD_UNLOCK(&mca_pls_bproc_component.lock);
    if(0 < mca_pls_bproc_component.debug) {
        opal_output(0, "in orte_pls_bproc_waitpid_daemon_cb, %d daemons left\n",
                    mca_pls_bproc_component.num_daemons);
    }
}

#ifdef MCA_pls_bproc_scyld
/**
 * compatibility function for scyld bproc and pre 3.2.0 LANL bproc. See the
 * bproc documentation for details
 */
static int bproc_vexecmove_io(int nnodes, int *nodes, int *pids,
                              struct bproc_io_t *io, int iolen, const char *cmd,
                              char * const argv[], char * envp[]) {
    int i;
    char * rank;

    OPAL_TRACE(1);
    
    for(i = 0; i < nnodes; i++) {
        pids[i] = fork();
        if(0 == pids[i]) {
            /* set BPROC_RANK so the proc can get its name */
            if (0 > asprintf(&rank, "%d", i)) {
                ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
                exit(-1);
            }
            opal_setenv("BPROC_RANK", rank, true, &envp);
            bproc_execmove_io(nodes[i], io, iolen, cmd, argv, envp);
            /* if we get here, there was an error */
            opal_show_help("help-pls-bproc.txt", "bproc-vexecmove-launch", true,
                           cmd, nodes[i], errno);
            ORTE_ERROR_LOG(ORTE_ERROR);
            exit(-1);
        } else if(-1 == pids[i]) {
            opal_show_help("help-pls-bproc.txt", "bproc-vexecmove-fork", true,
                           errno);
            ORTE_ERROR_LOG(ORTE_ERROR);
            return -1;
        }
    }
    return nnodes;
}

/**
 * compatibility function for scyld bproc and pre 3.2.0 LANL bproc. See the
 * bproc documentation for details
 */
static int bproc_vexecmove(int nnodes, int *nodes, int *pids, const char *cmd,
                           char * const argv[], char * envp[]) {
    return bproc_vexecmove_io(nnodes, nodes, pids, NULL, 0, cmd, argv, envp);
}
#endif

/**
 * Sets up the passed environment for processes launched by the bproc launcher.
 * @param env a pointer to the environment to setup
 */
static void orte_pls_bproc_setup_env(char *** env)
{
    char ** merged;
    char * var;
    char * param;
    int rc;
    int num_env;

    OPAL_TRACE(1);
    
    num_env = opal_argv_count(*env);
    /* append mca parameters to our environment */
    if(ORTE_SUCCESS != (rc = mca_base_param_build_env(env, &num_env, false))) {
        ORTE_ERROR_LOG(rc);
    }

    /* ns replica contact info */
    if(NULL == orte_process_info.ns_replica) {
        orte_ns.copy_process_name(&orte_process_info.ns_replica,
                                       orte_process_info.my_name);
        orte_process_info.ns_replica_uri = orte_rml.get_uri();
    }
    var = mca_base_param_environ_variable("ns","replica","uri");
    opal_setenv(var,orte_process_info.ns_replica_uri, true, env);
    free(var);

    /* make sure the frontend hostname does not get pushed out to the backend */
    var = mca_base_param_environ_variable("orte", "base", "nodename");
    opal_unsetenv(var, env);
    free(var);
    opal_unsetenv("HOSTNAME", env);

    /* make sure the username used to create the bproc directory is the same on
     * the backend as the frontend */
    var = mca_base_param_environ_variable("pls","bproc","username");
    opal_setenv(var, orte_system_info.user, true, env);
    free(var);

    /* gpr replica contact info */
    if(NULL == orte_process_info.gpr_replica) {
        orte_ns.copy_process_name(&orte_process_info.gpr_replica,
                                       orte_process_info.my_name);
        orte_process_info.gpr_replica_uri = orte_rml.get_uri();
    }
    var = mca_base_param_environ_variable("gpr","replica","uri");
    opal_setenv(var,orte_process_info.gpr_replica_uri, true, env);
    free(var);

    /* universe directory - needs to match orted */
    var = mca_base_param_environ_variable("universe", NULL, NULL);
    asprintf(&param, "%s@%s:%s", orte_universe_info.uid,
                orte_universe_info.host, orte_universe_info.name);
    opal_setenv(var, param, true, env);
    free(param);
    free(var);

    /* merge in environment */
    merged = opal_environ_merge(*env, environ);
    opal_argv_free(*env);
    *env = merged;

    /* make sure hostname doesn't get pushed to backend node */
    opal_unsetenv("HOSTNAME", env);
}

/**
 * Launches the daemons
 * @param cellid         the cellid of the job
 * @param envp           a pointer to the environment to use for the daemons
 * @param node_arrays    an array that holds the node arrays for each app context
 * @param node_array_lens an array of lengths of the node arrays
 * @param num_contexts   the number of application contexts
 * @param num_procs      the numer of processes in the job
 * @param global_vpid_start the starting vpid for the user's processes
 * @param jobid          the jobid for the user processes
 * @retval ORTE_SUCCESS
 * @retval error
 */
static int orte_pls_bproc_launch_daemons(orte_cellid_t cellid, char *** envp,
                                         int ** node_arrays, int * node_array_lens,
                                         int num_contexts, int num_procs,
                                         orte_vpid_t global_vpid_start,
                                         orte_jobid_t jobid, int *num_launched) {
    int * daemon_list = NULL;
    int num_nodes = 0;
    int num_daemons = 0;
    int rc, i, j;
    int * pids = NULL;
    int argc;
    char ** argv = NULL;
    char * param;
    char * var;
    int stride;
    char * orted_path;
    orte_jobid_t daemon_jobid;
    orte_process_name_t * proc_name;
    orte_vpid_t daemon_vpid_start = 0;
    orte_std_cntr_t idx;
    struct stat buf;
    opal_list_t daemons;
    orte_pls_daemon_info_t *dmn;
    opal_list_item_t *item;

    OPAL_TRACE(1);
    
    /* setup a list that will contain the info for all the daemons
     * so we can store it on the registry when done
     */
    OBJ_CONSTRUCT(&daemons, opal_list_t);

    /* find the length of the longest node array */
    for(i = 0; i < num_contexts; i++) {
        if(node_array_lens[i] > num_nodes) {
            num_nodes = node_array_lens[i];
        }
    }
    if(NULL == (daemon_list = (int*)malloc(sizeof(int) * num_nodes))) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    /* create a list of all the nodes that need daemons, which is all the nodes
     * that will have at least 1 process  */
    for(i = 0; i < num_nodes; i++) {
        for(j = 0; j < num_contexts; j++) {
            if(i < node_array_lens[j] && 0 < *(node_arrays[j] + i)) {
                daemon_list[num_daemons++] = i;
                break;
            }
        }
    }
    if(NULL == (pids = (int*)malloc(sizeof(int) * num_daemons))) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }

    /* allocate a range of vpids for the daemons */
    daemon_jobid = orte_process_info.my_name->jobid;
    rc = orte_ns.reserve_range(daemon_jobid, num_daemons, &daemon_vpid_start);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* setup the orted triggers for passing their launch info */
    if (ORTE_SUCCESS != (rc = orte_smr.init_orted_stage_gates(jobid, num_daemons, NULL, NULL))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* daemons calculate their process name using a "stride" of one, so
     * push that value into their environment */
    stride = 1;
    asprintf(&param, "%ld", (long)stride);
    var = mca_base_param_environ_variable("pls", "bproc", "stride");
    opal_setenv(var, param, true, envp);
    free(param);
    free(var);

    /* set up the base environment so the daemons can get their names once launched */
    rc = orte_ns_nds_bproc_put(cellid, daemon_jobid, daemon_vpid_start,
                               global_vpid_start, num_procs, envp);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    argc = 0;
    opal_argv_append(&argc, &argv, mca_pls_bproc_component.orted);
    /* check for debug flags */
#if 0
    if (mca_pls_bproc_component.debug) {
         opal_argv_append(&argc, &argv, "--debug");
         opal_argv_append(&argc, &argv, "--debug-daemons");
    }
#endif 

    opal_argv_append(&argc, &argv, "--bootproxy");
    orte_ns.convert_jobid_to_string(&param, jobid);
    opal_argv_append(&argc, &argv, param);
    free(param);

    /* pass along the universe name and location info */
    opal_argv_append(&argc, &argv, "--universe");
    asprintf(&param, "%s@%s:%s", orte_universe_info.uid,
                orte_universe_info.host, orte_universe_info.name);
    opal_argv_append(&argc, &argv, param);
    free(param);

    /* tell orted not to demonize itself */
    opal_argv_append(&argc, &argv, "--no-daemonize");

    /* find orted */
    if(0 == stat(mca_pls_bproc_component.orted, &buf)) {
        orted_path = strdup(mca_pls_bproc_component.orted);
    } else {
        orted_path = opal_path_findv(mca_pls_bproc_component.orted, 0, environ, NULL);
        if(NULL == orted_path) {
            orted_path = opal_os_path( false, OPAL_BINDIR, mca_pls_bproc_component.orted, NULL );
            if( (NULL != orted_path) || (0 != stat(orted_path, &buf)) ) {
                char *path = getenv("PATH");
                if (NULL == path) {
                    path = ("PATH is empty!");
                }
                opal_show_help("help-pls-bproc.txt", "no-orted", true,
                               mca_pls_bproc_component.orted,
                               mca_pls_bproc_component.orted, path, OPAL_BINDIR);
                rc = ORTE_ERROR;
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
    }

    if(0 < mca_pls_bproc_component.debug) {
        opal_output(0, "PLS_BPROC DEBUG: launching %d daemons. cmd: %s ",
                    num_daemons, orted_path);
    }

    /* launch the daemons */
    mca_pls_bproc_component.num_daemons += num_daemons;
    rc = bproc_vexecmove(num_daemons, daemon_list, pids, orted_path, argv, *envp);
    if(rc != num_daemons) {
        opal_show_help("help-pls-bproc.txt", "daemon-launch-number", true,
                       num_daemons, rc, orted_path);
        mca_pls_bproc_component.num_daemons -= num_daemons;
        rc = ORTE_ERROR;
        goto cleanup;
    }
    if(0 < mca_pls_bproc_component.debug) {
        opal_output(0, "PLS_BPROC DEBUG: %d daemons launched. First pid: %d\n",
                    rc, *pids);
    }

    for(i = 0; i < num_daemons; i++) {
        if(0 >= pids[i]) {
            opal_show_help("help-pls-bproc.txt", "daemon-launch-bad-pid", true,
                           daemon_list[i], pids[i], errno, orted_path);
            rc = ORTE_ERROR;
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        } else {
            rc = orte_ns.create_process_name(&proc_name, cellid, daemon_jobid,
                                             daemon_vpid_start + i);
            if(ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            rc = orte_pointer_array_add(&idx, mca_pls_bproc_component.daemon_names,
                                        proc_name);
            if(ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            if (0 > asprintf(&param, "%d", daemon_list[i])) {
                rc = ORTE_ERR_OUT_OF_RESOURCE;
                ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
                goto cleanup;
            }
            rc = orte_pls_bproc_set_node_pid(cellid, param, jobid, pids[i]);
            if(ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
            dmn = OBJ_NEW(orte_pls_daemon_info_t);
            orte_dss.copy((void**)&(dmn->name), proc_name, ORTE_NAME);
            dmn->cell = cellid;
            dmn->nodename = strdup(param);
            dmn->active_job = jobid;
            opal_list_append(&daemons, &dmn->super);
            
            free(param);
            
            rc = orte_wait_cb(pids[i], orte_pls_bproc_waitpid_daemon_cb,
                              &daemon_list[i]);
            if(ORTE_SUCCESS != rc) {
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            }
        }
    }
    /* store the daemon info */
    if (ORTE_SUCCESS != (rc = orte_pls_base_store_active_daemons(&daemons, jobid))) {
        ORTE_ERROR_LOG(rc);
    }
    *num_launched = num_daemons;

cleanup:
    if(NULL != argv) {
        opal_argv_free(argv);
    }
    if(NULL != pids) {
        free(pids);
    }
    if(NULL != orted_path) {
        free(orted_path);
    }
    while (NULL != (item = opal_list_remove_first(&daemons))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&daemons);
    
    return rc;
}


static void
orte_pls_bproc_check_node_state(orte_gpr_notify_data_t *notify_data,
                                void *user_tag) 
{ 
    orte_gpr_value_t **values;
    bool dead_node = false;
    char *dead_node_name; 
    orte_std_cntr_t i, j;
    
    OPAL_TRACE(1);
        
    /* first see if node is in 
       ORTE_NODE_STATE_DOWN or 
       ORTE_NODE_STATE_REBOOT */
    
    values = (orte_gpr_value_t**)(notify_data->values)->addr;
    for( j = 0; j < notify_data->cnt; j++) { 
        dead_node = false;
        for( i = 0; i < values[j]->cnt; i++) { 
            orte_gpr_keyval_t* keyval = values[j]->keyvals[i];
            if(strcmp(keyval->key, ORTE_NODE_STATE_KEY) == 0) { 
                orte_node_state_t *node_state;
                int ret;
                if( ORTE_SUCCESS != (ret = orte_dss.get( (void **) &node_state, keyval->value, ORTE_NODE_STATE))) { 
                    return;
                }
                if( *node_state == ORTE_NODE_STATE_DOWN || 
                    *node_state == ORTE_NODE_STATE_REBOOT) { 
                    dead_node = true;
                    printf("found a  dead node state.. \n");
                }
            } else if(strcmp(keyval->key, ORTE_NODE_NAME_KEY) == 0) { 
                char* tmp_name;
                int ret;
                if( ORTE_SUCCESS != (ret = orte_dss.get( (void **) &tmp_name, keyval->value, ORTE_STRING))) { 
                    return;
                }
                else { 
                    dead_node_name = strdup(tmp_name);
                    printf("found a node named %s\n", dead_node_name);
                }
            }
        }
        printf("found a node named %s is dead?  %d\n", dead_node_name, dead_node);
        if(dead_node) {
            /* gotta see if this node belongs to us... arg.. */
            /* also, we know by order of creation that the node state */ 
            /* comes before the node name.. see smr_bproc.c */
            orte_std_cntr_t name_idx;
            for (name_idx = 0;
                 name_idx < orte_pointer_array_get_size(mca_pls_bproc_component.active_node_names);
                 name_idx++) { 
                char* node_name = (char*) orte_pointer_array_get_item(mca_pls_bproc_component.active_node_names, name_idx);
                if(strcmp(node_name, dead_node_name) == 0){ 
                    /* one of our nodes up and died... */
                    /* not much to do other than die.... */
                    int ret = ORTE_SUCCESS;
                    char *segment = NULL;
                    orte_gpr_value_t** seg_values = NULL;
                    orte_std_cntr_t  num_values = 0;
	      
                    /**********************
                     * Job Info segment
                     **********************/
                    segment = strdup(ORTE_JOBINFO_SEGMENT);

                    if( ORTE_SUCCESS != (ret = orte_gpr.get(ORTE_GPR_KEYS_OR|ORTE_GPR_TOKENS_OR,
                                                            segment,
                                                            NULL,
                                                            NULL,
                                                            &num_values,
                                                            &seg_values ) ) ) {
		
                    }
	      
                    /*
                     * kill all the jobids that are not zero
                     */
                    for(i = 0; i < num_values; ++i) {
                        orte_gpr_value_t* value = values[i];
                        orte_jobid_t jobid;
                        orte_schema.extract_jobid_from_segment_name(&jobid, value->tokens[0]);
                        printf("killing jobid %d\n", jobid);
                        if(jobid != 0) 
                            orte_pls_bproc_terminate_job(jobid);
                    }
                    /*
                     * and kill everyone else 
                     */
                    printf("and go bye-bye...\n");
                    orte_pls_bproc_terminate_job(0); 
                    /* shouldn't ever get here.. */
                    exit(1);
                }


            }
        }
    }
}


static int
orte_pls_bproc_monitor_nodes(void) 
{
    orte_gpr_subscription_id_t id;

    OPAL_TRACE(1);

    return orte_gpr.subscribe_1(&id,
                                NULL,
                                NULL,
                                ORTE_GPR_NOTIFY_VALUE_CHG,
                                ORTE_GPR_TOKENS_OR | 
                                ORTE_GPR_KEYS_OR,
                                ORTE_NODE_SEGMENT,
                                NULL,
                                ORTE_NODE_STATE_KEY,
                                orte_pls_bproc_check_node_state,
				NULL);
}


/**
 * Launches the application processes
 * @param cellid         the cellid of the job
 * @param jobid          the jobid of the job
 * @param map            a pointer to the mapping of this application
 * @param num_processes  the number of processes in this job
 * @param vpid_start     the starting vpid for this app context
 * @param global_vpid_start the starting vpid for the user's processes
 * @param app_context    the application context number
 * @param node_array     the node array for this context
 * @param node_array_len the length of the node array
 * @retval ORTE_SUCCESS
 * @retval error
 */
static int orte_pls_bproc_launch_app(orte_cellid_t cellid, orte_jobid_t jobid,
                                     orte_rmaps_base_map_t* map, int num_processes,
                                     orte_vpid_t vpid_start,
                                     orte_vpid_t global_vpid_start,
                                     int app_context, int * node_array,
                                     int node_array_len) {
    int * node_list = NULL;
    int num_nodes, num_slots, cycle;
    int rc, i, j, stride;
    int * pids = NULL;
    char * var, * param;
    orte_process_name_t * proc_name;
    struct bproc_io_t bproc_io[3];
    orte_rmaps_base_node_t *node;

    OPAL_TRACE(1);
    
    if(NULL == (pids = (int*)malloc(sizeof(int) * node_array_len))) {
        ORTE_ERROR_LOG(ORTE_ERR_OUT_OF_RESOURCE);
        return ORTE_ERR_OUT_OF_RESOURCE;
    }

    /* set up app context */
    asprintf(&param, "%d", app_context);
    var = mca_base_param_environ_variable("pls", "bproc", "app_context");
    opal_setenv(var, param, true, &map->app->env);
    free(param);
    free(var);

    /* in order for bproc processes to properly compute their name,
     * we have to provide them with info on the number of slots
     * on each node (which is a constant in bproc). We will pass this
     * in an appropriate parameter which we set for each app_context
     */
    node = (orte_rmaps_base_node_t*)opal_list_get_first(&map->nodes);
    if (NULL == node) {
        ORTE_ERROR_LOG(ORTE_ERROR);
        return ORTE_ERROR;
    }
    num_slots = node->node->node_slots;
    
    /* set the vpid-to-vpid stride based on the mapping mode */
    if (mca_pls_bproc_component.bynode) {
        /* we are mapping by node, so we want to set the stride
         * length (i.e., the step size between vpids that is used
         * to compute the process name) to 1
         */
        stride = 1;
    } else {
        /* we are mapping by slot, so we want to set the stride
        * length (i.e., the step size between vpids that is used
        * to compute the process name) to the number of slots
        */
        stride = num_slots;
    }
    /* and push that value into the process' environment */
    asprintf(&param, "%ld", (long)stride);
    var = mca_base_param_environ_variable("pls", "bproc", "stride");
    opal_setenv(var, param, true, &map->app->env);
    free(param);
    free(var);
    
    /* initialize the cycle count. Computing the process name under Bproc
     * is a complex matter when mapping by slot as Bproc's inherent
     * methodology is to do everything by node. When mapping by slot, the
     * first num_slots number of launch cycles all have a vpid_start that
     * will differ by one - i.e., the processes on a given node will have
     * vpids that differ by only one.
     *
     * However, when we oversubscribe, we enter into a cyclic arrangement.
     * During each cycle, the above description of how names are assigned
     * is accurate. However, each cycle (i.e., each collection of num_nodes
     * processes that we launch) will have a vpid start that is offset by
     * num_slots * num_nodes. We have to compensate for that here when we
     * calculate and pass the vpid_start param so that the processes can
     * correctly compute their name
     */
    cycle = 1;
    
    /* launch the processes */
    i = 1;
    rc = orte_pls_bproc_node_list(node_array, node_array_len, &node_list,
                                  &num_nodes, i);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    while(0 != num_nodes) {

        /* setup environment so the procs can figure out their names */
        rc = orte_ns_nds_bproc_put(cellid, jobid, vpid_start, global_vpid_start,
                                   num_processes, &map->app->env);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }

        rc = orte_pls_bproc_setup_io(jobid, bproc_io, i - 1, app_context);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        if(0 < mca_pls_bproc_component.debug) {
            opal_output(0, "pls_bproc: launching %d processes", num_nodes);
        }
        rc = bproc_vexecmove_io(num_nodes, node_list, pids, bproc_io, 3,
                                map->app->app, map->app->argv, map->app->env);
        if(0 < mca_pls_bproc_component.debug) {
            opal_output(0, "pls_bproc: %d processes launched. First pid: %d",
                        rc, *pids);
        }
        if(rc != num_nodes) {
            opal_show_help("help-pls-bproc.txt", "proc-launch-number", true,
                           num_nodes, rc, map->app->app);
            rc = ORTE_ERROR;
            goto cleanup;
        }
        for(j = 0; j < num_nodes; j++) {
            if(0 >= pids[j]) {
                opal_show_help("help-pls-bproc.txt", "proc-launch-bad-pid", true,
                               node_list[j], pids[j], errno, map->app->app);
                rc = ORTE_ERROR;
                ORTE_ERROR_LOG(rc);
                goto cleanup;
            } else {
                mca_pls_bproc_component.num_procs++;
                rc = orte_ns.create_process_name(&proc_name, cellid, jobid,
                                                 vpid_start + j);
                if(ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                    goto cleanup;
                }
                orte_pls_bproc_set_proc_pid(proc_name, pids[j]);
                if(ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                    goto cleanup;
                }
                rc = orte_wait_cb(pids[j], orte_pls_bproc_waitpid_cb, proc_name);
                if(ORTE_SUCCESS != rc) {
                    ORTE_ERROR_LOG(rc);
                    goto cleanup;
                }
            }
        }
        free(node_list);
        node_list = NULL;
        i++;
        if (mca_pls_bproc_component.bynode) {
            /* we are mapping by node, so the vpid_start must increment by
             * the number of nodes
             */
            vpid_start += num_nodes;
        } else {
            /* we are mapping by slot. Here is where we need to check our
             * cyclic condition - if we are at the end of a cycle, then
             * we need to increment the vpid_start by num_slots*num_nodes.
             * Otherwise, we just increment it by one.
             */
            if (cycle == num_slots) {
                 /* end of cycle condition */
                vpid_start += num_slots * num_nodes - 1;
                cycle = 1;
            } else {
                vpid_start += 1;
                cycle++;
            }
        }
        
        rc = orte_pls_bproc_node_list(node_array, node_array_len, &node_list,
                                      &num_nodes, i);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
    }

cleanup:
    if(NULL != node_list) {
        free(node_list);
    }
    if(NULL != pids) {
        free(pids);
    }
    return rc;
}

/**
 * The main bproc launcher. See pls_bproc.h for a high level overview of how
 * the bproc launching works.
 * Here we:
 * -# Launch the deamons on the backend nodes.
 * -# The daemons setup files for io forwarding then connect back to us to
 *     tells us they are ready for the actual apps.
 * -# Launch the apps on the backend nodes
 *
 * @param jobid the jobid of the job to launch
 * @retval ORTE_SUCCESS
 * @retval error
 */
int orte_pls_bproc_launch(orte_jobid_t jobid) {
    opal_list_item_t* item, *item2;
    opal_list_t mapping;
    orte_cellid_t cellid;
    orte_rmaps_base_map_t* map;
    orte_vpid_t vpid_launch;
    orte_vpid_t vpid_range;
    orte_vpid_t vpid_start;
    int rc;
    int src[4];
    int ** node_array = NULL;
    int *  node_array_len = NULL;
    int num_processes = 0;
    int num_daemons = 0;
    int context = 0;
    int j;
    orte_std_cntr_t idx;
    char cwd_save[OMPI_PATH_MAX + 1];

    OPAL_TRACE(1);
    
    /* make sure the pls_bproc receive function has been started */
    if (ORTE_SUCCESS != (rc = orte_pls_bproc_comm_start())) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    
    /* save the current working directory */
    if (NULL == getcwd(cwd_save, sizeof(cwd_save))) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
        return ORTE_ERR_NOT_FOUND;
    }
    cwd_save[sizeof(cwd_save) - 1] = '\0';
    
    /* query for the application context and allocated nodes */
    OBJ_CONSTRUCT(&mapping, opal_list_t);
    if(ORTE_SUCCESS != (rc = orte_rmaps_base_get_map(jobid, &mapping))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }
    if(ORTE_SUCCESS != (rc = orte_rmgr.get_vpid_range(jobid, &vpid_start,
                                                      &vpid_range))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* get the cellid */
    cellid = orte_process_info.my_name->cellid;
    
    /* do a large lock so the processes will not decrement the process count
     * until we are done launching */

    for (item =  opal_list_get_first(&mapping);
         item != opal_list_get_end(&mapping);
         item =  opal_list_get_next(item)) {
        orte_std_cntr_t i;
        map = (orte_rmaps_base_map_t*) item;
        
        orte_dss.dump(0, map, ORTE_JOB_MAP);
        
        for (i = 0; i < map->num_procs; ++i) {
            orte_app_context_t *context = map->app;

            /* Check that the cwd is sane.  We have to chdir there in
               to check the executable, because the executable could
               have been specified as a relative path to the wdir */
            rc = orte_rmgr.check_context_cwd(context, true);
            if (ORTE_SUCCESS != rc) {
                goto cleanup;
            }

            /* Check that the app exists and is executable */
            rc = orte_rmgr.check_context_app(context);
            if (ORTE_SUCCESS != rc) {
                goto cleanup;
            }

            /* Return to the original dir */
            if (0 != chdir(cwd_save)) {
                rc = ORTE_ERR_IN_ERRNO;
                goto cleanup;
            }
        }
    }

    if(0 < mca_pls_bproc_component.debug) {
        opal_output(0, "pls_bproc: --- starting to launch procs ---");
    }

    /* create an array to hold the pointers to the node arrays for each app
     * context. Also, create an array to hold the lengths of the node arrays */
    node_array = malloc(opal_list_get_size(&mapping) * sizeof(int *));
    node_array_len = malloc(opal_list_get_size(&mapping) * sizeof(int *));

    /* for each application context - create a node array and setup its env */
    for(item =  opal_list_get_first(&mapping);
        item != opal_list_get_end(&mapping);
        item =  opal_list_get_next(item)) {
        map = (orte_rmaps_base_map_t*)item;
        rc = orte_pls_bproc_node_array(map, &node_array[context],
                                       &node_array_len[context]);
        if(0 > rc) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        orte_pls_bproc_setup_env(&map->app->env);
        num_processes += rc;
        context++;
    }
    
    idx = 0;
    for (item =  opal_list_get_first(&mapping);
	 item != opal_list_get_end(&mapping);
	 item =  opal_list_get_next(item)) {
      orte_rmaps_base_map_t *map = (orte_rmaps_base_map_t*) item;
      for (item2 =  opal_list_get_first(&map->nodes);
	   item2 != opal_list_get_end(&map->nodes);
	   item2 =  opal_list_get_next(item2)) {
	orte_ras_node_t* node = (orte_ras_node_t*) item2;
	
	rc = orte_pointer_array_add(&idx, mca_pls_bproc_component.active_node_names,
				    strdup(node->node_name));
	
	
      }
    }
    
    /* setup subscription for each node so we can detect 
       when the node's state changes, usefull for aborting when 
       a bproc node up and dies */ 
    
    rc = orte_pls_bproc_monitor_nodes(); 
    
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* launch the daemons on all the nodes which have processes assign to them */
    rc = orte_pls_bproc_launch_daemons(cellid, &map->app->env, node_array,
                                       node_array_len, context, num_processes,
                                       vpid_start, jobid, &num_daemons);
    if(ORTE_SUCCESS != rc) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }

    /* wait for communication back from the daemons, which indicates they have
     * sucessfully set up the pty/pipes and IO forwarding which the user apps
     * will use  */
    for(j = 0; j < num_daemons; j++) {
        orte_buffer_t ack;
        OBJ_CONSTRUCT(&ack, orte_buffer_t);
        rc = mca_oob_recv_packed(ORTE_RML_NAME_ANY, &ack, ORTE_RML_TAG_BPROC);
        if(0 > rc) {
            ORTE_ERROR_LOG(rc);
            OBJ_DESTRUCT(&ack);
            goto cleanup;
        }
        idx = 4;
        rc = orte_dss.unpack(&ack, &src, &idx, ORTE_INT);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
        }
        OBJ_DESTRUCT(&ack);

        if(-1 == src[0]) {
            /* one of the daemons has failed to properly launch. The error is sent
             * by orte_pls_bproc_waitpid_daemon_cb  */
            if(-1 == src[1]) { /* did not die on a signal */
                opal_show_help("help-pls-bproc.txt", "daemon-died-no-signal", true,
                               src[2], src[3]);
            } else { /* died on a signal */
                opal_show_help("help-pls-bproc.txt", "daemon-died-signal", true,
                               src[2], src[3], src[1]);
            }
            rc = ORTE_ERROR;
            ORTE_ERROR_LOG(rc);
            orte_pls_bproc_terminate_job(jobid);
            goto cleanup;
        }
    }

    context = 0;
    vpid_launch = vpid_start;
    /* for each application context launch the app */
    for(item =  opal_list_get_first(&mapping);
        item != opal_list_get_end(&mapping);
        item =  opal_list_get_next(item)) {
        map = (orte_rmaps_base_map_t*)item;

        rc = orte_rmgr.check_context_cwd(map->app, true);
        if (ORTE_SUCCESS != rc) {
            goto cleanup;
        }

        rc = orte_pls_bproc_launch_app(cellid, jobid, map, num_processes,
                                    vpid_launch, vpid_start, map->app->idx,
                                    node_array[context], node_array_len[context]);
        if(ORTE_SUCCESS != rc) {
            ORTE_ERROR_LOG(rc);
            goto cleanup;
        }
        free(node_array[context]);
        context++;
        vpid_launch = vpid_start + mca_pls_bproc_component.num_procs;
    }

    mca_pls_bproc_component.done_launching = true;
cleanup:
    chdir(cwd_save);
    while(NULL != (item = opal_list_remove_first(&mapping))) {
        OBJ_RELEASE(item);
    }
    if(NULL != node_array) {
        free(node_array);
    }
    if(NULL != node_array_len) {
        free(node_array_len);
    }
    OBJ_DESTRUCT(&mapping);
    return rc;
}

/**
 * Terminate all processes associated with this job */
int orte_pls_bproc_terminate_job(orte_jobid_t jobid) {
    pid_t* pids;
    orte_std_cntr_t i, num_pids;
    int rc;
    
    OPAL_TRACE(1);
    
    if(0 < mca_pls_bproc_component.debug) {
        opal_output(0, "orte_pls_bproc: terminating job %ld", jobid);
    }
    
    /* kill application process */
    if(ORTE_SUCCESS != (rc = orte_pls_bproc_get_proc_pids(jobid, &pids, &num_pids)))
        return rc;
    for(i=0; i<num_pids; i++) {
        if(mca_pls_bproc_component.debug) {
            opal_output(0, "orte_pls_bproc: killing proc: %d\n", pids[i]);
        }
        kill(pids[i], mca_pls_bproc_component.terminate_sig);
    }
    if(NULL != pids)
        free(pids);

    /* dont kill daemons - mpirun will do this for us */
    return ORTE_SUCCESS;
}

/**
* Terminate the orteds for a given job
 */
int orte_pls_bproc_terminate_orteds(orte_jobid_t jobid)
{
    int rc;
    opal_list_t daemons;
    opal_list_item_t *item;

    OPAL_TRACE(1);
    
    /* construct the list of active daemons on this job */
    OBJ_CONSTRUCT(&daemons, opal_list_t);
    if (ORTE_SUCCESS != (rc = orte_pls_base_get_active_daemons(&daemons, jobid))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }

    /* now tell them to die! */
    if (ORTE_SUCCESS != (rc = orte_pls_base_orted_exit(&daemons))) {
        ORTE_ERROR_LOG(rc);
    }

CLEANUP:
    while (NULL != (item = opal_list_remove_first(&daemons))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&daemons);
    return rc;
}

/**
 * Terminate a specific process.
 */
int orte_pls_bproc_terminate_proc(const orte_process_name_t* proc_name) {
    int rc;
    pid_t pid;

    OPAL_TRACE(1);
    
    if(ORTE_SUCCESS != (rc = orte_pls_bproc_get_proc_pid(proc_name, &pid)))
        return rc;
    if(kill(pid, mca_pls_bproc_component.terminate_sig) != 0) {
        switch(errno) {
            case EINVAL:
                return ORTE_ERR_BAD_PARAM;
            case ESRCH:
                return ORTE_ERR_NOT_FOUND;
            case EPERM:
                return ORTE_ERR_PERM;
            default:
                return ORTE_ERROR;
        }
    }
    return ORTE_SUCCESS;
}

/**
 * Signal all processes associated with this job
 */
int orte_pls_bproc_signal_job(orte_jobid_t jobid, int32_t signal) {
    pid_t* pids;
    orte_std_cntr_t i, num_pids;
    int rc;

    OPAL_TRACE(1);
    
    /* signal application process */
    if(ORTE_SUCCESS != (rc = orte_pls_bproc_get_proc_pids(jobid, &pids, &num_pids)))
        return rc;
    for(i=0; i<num_pids; i++) {
        if(mca_pls_bproc_component.debug) {
            opal_output(0, "orte_pls_bproc: signaling proc: %d\n", pids[i]);
        }
        kill(pids[i], (int)signal);
    }
    if(NULL != pids)
        free(pids);

    /** dont signal daemons - this is strictly for signalling application processes */
    return ORTE_SUCCESS;
}

/**
 * Signal a specific process.
 */
int orte_pls_bproc_signal_proc(const orte_process_name_t* proc_name, int32_t signal) {
    int rc;
    pid_t pid;

    OPAL_TRACE(1);
    
    if(ORTE_SUCCESS != (rc = orte_pls_bproc_get_proc_pid(proc_name, &pid)))
        return rc;
    if(kill(pid, (int)signal) != 0) {
        switch(errno) {
            case EINVAL:
                return ORTE_ERR_BAD_PARAM;
            case ESRCH:
                return ORTE_ERR_NOT_FOUND;
            case EPERM:
                return ORTE_ERR_PERM;
            default:
                return ORTE_ERROR;
        }
    }
    return ORTE_SUCCESS;
}

/**
 * Module cleanup
 */
int orte_pls_bproc_finalize(void)
{
   return ORTE_SUCCESS;
}


/*
 * Handle threading issues.
 */

#if OMPI_HAVE_POSIX_THREADS && OMPI_THREADS_HAVE_DIFFERENT_PIDS

struct orte_pls_bproc_stack_t {
    opal_condition_t cond;
    opal_mutex_t mutex;
    bool complete;
    orte_jobid_t jobid;
    int rc;
};
typedef struct orte_pls_bproc_stack_t orte_pls_bproc_stack_t;

static void orte_pls_bproc_stack_construct(orte_pls_bproc_stack_t* stack)
{
    OBJ_CONSTRUCT(&stack->mutex, opal_mutex_t);
    OBJ_CONSTRUCT(&stack->cond, opal_condition_t);
    stack->rc = 0;
    stack->complete = false;
}

static void orte_pls_bproc_stack_destruct(orte_pls_bproc_stack_t* stack)
{
    OBJ_DESTRUCT(&stack->mutex);
    OBJ_DESTRUCT(&stack->cond);
}

static OBJ_CLASS_INSTANCE(
    orte_pls_bproc_stack_t,
    opal_object_t,
    orte_pls_bproc_stack_construct,
    orte_pls_bproc_stack_destruct);


static void orte_pls_bproc_launch_cb(int fd, short event, void* args)
{

    orte_pls_bproc_stack_t *stack = (orte_pls_bproc_stack_t*)args;
    stack->rc = orte_pls_bproc_launch(stack->jobid);
    OPAL_THREAD_LOCK(&stack->mutex);
    stack->complete = true;
    opal_condition_signal(&stack->cond);
    OPAL_THREAD_UNLOCK(&stack->mutex);
}

int orte_pls_bproc_launch_threaded(orte_jobid_t jobid)
{
    struct timeval tv = { 0, 0 };
    struct opal_event event;
    struct orte_pls_bproc_stack_t stack;

    OBJ_CONSTRUCT(&stack, orte_pls_bproc_stack_t);

    stack.jobid = jobid;
    opal_evtimer_set(&event, orte_pls_bproc_launch_cb, &stack);
    opal_evtimer_add(&event, &tv);

    OPAL_THREAD_LOCK(&stack.mutex);
    while(stack.complete == false)
         opal_condition_wait(&stack.cond, &stack.mutex);
    OPAL_THREAD_UNLOCK(&stack.mutex);
    OBJ_DESTRUCT(&stack);
    return stack.rc;
}

#endif

