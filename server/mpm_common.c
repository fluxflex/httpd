/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* The purpose of this file is to store the code that MOST mpm's will need
 * this does not mean a function only goes into this file if every MPM needs
 * it.  It means that if a function is needed by more than one MPM, and
 * future maintenance would be served by making the code common, then the
 * function belongs here.
 *
 * This is going in src/main because it is not platform specific, it is
 * specific to multi-process servers, but NOT to Unix.  Which is why it
 * does not belong in src/os/unix
 */

#include "apr.h"
#include "apr_thread_proc.h"
#include "apr_signal.h"
#include "apr_strings.h"
#define APR_WANT_STRFUNC
#include "apr_want.h"
#include "apr_getopt.h"
#include "apr_optional.h"
#include "apr_allocator.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_main.h"
#include "mpm_common.h"
#include "ap_mpm.h"
#include "ap_listen.h"
#include "util_mutex.h"

#include "scoreboard.h"

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#if APR_HAVE_UNISTD_H
#include <unistd.h>
#endif

APLOG_USE_MODULE(core);

#if AP_ENABLE_EXCEPTION_HOOK
APR_HOOK_STRUCT(
    APR_HOOK_LINK(fatal_exception)
    APR_HOOK_LINK(monitor)
    APR_HOOK_LINK(drop_privileges)
    APR_HOOK_LINK(mpm)
    APR_HOOK_LINK(mpm_query)
    APR_HOOK_LINK(mpm_register_timed_callback)
    APR_HOOK_LINK(mpm_get_name)
    APR_HOOK_LINK(end_generation)
    APR_HOOK_LINK(child_status)
)
AP_IMPLEMENT_HOOK_RUN_ALL(int, fatal_exception,
                          (ap_exception_info_t *ei), (ei), OK, DECLINED)
#else
APR_HOOK_STRUCT(
    APR_HOOK_LINK(monitor)
    APR_HOOK_LINK(drop_privileges)
    APR_HOOK_LINK(mpm)
    APR_HOOK_LINK(mpm_query)
    APR_HOOK_LINK(mpm_register_timed_callback)
    APR_HOOK_LINK(mpm_get_name)
    APR_HOOK_LINK(end_generation)
    APR_HOOK_LINK(child_status)
)
#endif
AP_IMPLEMENT_HOOK_RUN_ALL(int, monitor,
                          (apr_pool_t *p, server_rec *s), (p, s), OK, DECLINED)
AP_IMPLEMENT_HOOK_RUN_ALL(int, drop_privileges,
                          (apr_pool_t * pchild, server_rec * s),
                          (pchild, s), OK, DECLINED)
AP_IMPLEMENT_HOOK_RUN_FIRST(int, mpm,
                            (apr_pool_t *pconf, apr_pool_t *plog, server_rec *s),
                            (pconf, plog, s), DECLINED)
AP_IMPLEMENT_HOOK_RUN_FIRST(int, mpm_query,
                            (int query_code, int *result, apr_status_t *_rv),
                            (query_code, result, _rv), DECLINED)
AP_IMPLEMENT_HOOK_RUN_FIRST(apr_status_t, mpm_register_timed_callback,
                            (apr_time_t t, ap_mpm_callback_fn_t *cbfn, void *baton),
                            (t, cbfn, baton), APR_ENOTIMPL)
AP_IMPLEMENT_HOOK_VOID(end_generation,
                       (server_rec *s, ap_generation_t gen),
                       (s, gen))
AP_IMPLEMENT_HOOK_VOID(child_status,
                       (server_rec *s, pid_t pid, ap_generation_t gen, int slot, mpm_child_status status),
                       (s,pid,gen,slot,status))

/* hooks with no args are implemented last, after disabling APR hook probes */
#if defined(APR_HOOK_PROBES_ENABLED)
#undef APR_HOOK_PROBES_ENABLED
#undef APR_HOOK_PROBE_ENTRY
#define APR_HOOK_PROBE_ENTRY(ud,ns,name,args)
#undef APR_HOOK_PROBE_RETURN
#define APR_HOOK_PROBE_RETURN(ud,ns,name,rv,args)
#undef APR_HOOK_PROBE_INVOKE
#define APR_HOOK_PROBE_INVOKE(ud,ns,name,src,args)
#undef APR_HOOK_PROBE_COMPLETE
#define APR_HOOK_PROBE_COMPLETE(ud,ns,name,src,rv,args)
#undef APR_HOOK_INT_DCL_UD
#define APR_HOOK_INT_DCL_UD
#endif
AP_IMPLEMENT_HOOK_RUN_FIRST(const char *, mpm_get_name,
                            (void),
                            (), NULL)

typedef struct mpm_gen_info_t {
    APR_RING_ENTRY(mpm_gen_info_t) link;
    int gen;          /* which gen? */
    int active;       /* number of active processes */
} mpm_gen_info_t;

APR_RING_HEAD(mpm_gen_info_head_t, mpm_gen_info_t);
static struct mpm_gen_info_head_t geninfo, unused_geninfo;
static int gen_head_init; /* yuck */

/* variables representing config directives implemented here */
const char *ap_pid_fname;
int ap_max_requests_per_child;
char ap_coredump_dir[MAX_STRING_LEN];
int ap_coredumpdir_configured;
int ap_graceful_shutdown_timeout;
apr_uint32_t ap_max_mem_free;
apr_size_t ap_thread_stacksize;

/* Set defaults for config directives implemented here.  This is
 * called from core's pre-config hook, so MPMs which need to override
 * one of these should run their pre-config hook after that of core.
 */
void mpm_common_pre_config(apr_pool_t *pconf)
{
    ap_pid_fname = DEFAULT_PIDLOG;
    ap_max_requests_per_child = 0; /* unlimited */
    apr_cpystrn(ap_coredump_dir, ap_server_root, sizeof(ap_coredump_dir));
    ap_coredumpdir_configured = 0;
    ap_graceful_shutdown_timeout = 0; /* unlimited */
    ap_max_mem_free = APR_ALLOCATOR_MAX_FREE_UNLIMITED;
    ap_thread_stacksize = 0; /* use system default */
}

/* number of calls to wait_or_timeout between writable probes */
#ifndef INTERVAL_OF_WRITABLE_PROBES
#define INTERVAL_OF_WRITABLE_PROBES 10
#endif
static int wait_or_timeout_counter;

void ap_wait_or_timeout(apr_exit_why_e *status, int *exitcode, apr_proc_t *ret,
                        apr_pool_t *p, server_rec *s)
{
    apr_status_t rv;

    ++wait_or_timeout_counter;
    if (wait_or_timeout_counter == INTERVAL_OF_WRITABLE_PROBES) {
        wait_or_timeout_counter = 0;
        ap_run_monitor(p, s);
    }

    rv = apr_proc_wait_all_procs(ret, exitcode, status, APR_NOWAIT, p);
    if (APR_STATUS_IS_EINTR(rv)) {
        ret->pid = -1;
        return;
    }

    if (APR_STATUS_IS_CHILD_DONE(rv)) {
        return;
    }

    apr_sleep(apr_time_from_sec(1));
    ret->pid = -1;
    return;
}

#if defined(TCP_NODELAY)
void ap_sock_disable_nagle(apr_socket_t *s)
{
    /* The Nagle algorithm says that we should delay sending partial
     * packets in hopes of getting more data.  We don't want to do
     * this; we are not telnet.  There are bad interactions between
     * persistent connections and Nagle's algorithm that have very severe
     * performance penalties.  (Failing to disable Nagle is not much of a
     * problem with simple HTTP.)
     *
     * In spite of these problems, failure here is not a shooting offense.
     */
    apr_status_t status = apr_socket_opt_set(s, APR_TCP_NODELAY, 1);

    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, status, ap_server_conf,
                     "apr_socket_opt_set: (TCP_NODELAY)");
    }
}
#endif

#ifdef HAVE_GETPWNAM
AP_DECLARE(uid_t) ap_uname2id(const char *name)
{
    struct passwd *ent;

    if (name[0] == '#')
        return (atoi(&name[1]));

    if (!(ent = getpwnam(name))) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "%s: bad user name %s", ap_server_argv0, name);
        exit(1);
    }

    return (ent->pw_uid);
}
#endif

#ifdef HAVE_GETGRNAM
AP_DECLARE(gid_t) ap_gname2id(const char *name)
{
    struct group *ent;

    if (name[0] == '#')
        return (atoi(&name[1]));

    if (!(ent = getgrnam(name))) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP, 0, NULL,
                     "%s: bad group name %s", ap_server_argv0, name);
        exit(1);
    }

    return (ent->gr_gid);
}
#endif

#ifndef HAVE_INITGROUPS
int initgroups(const char *name, gid_t basegid)
{
#if defined(_OSD_POSIX) || defined(OS2) || defined(WIN32) || defined(NETWARE)
    return 0;
#else
    gid_t groups[NGROUPS_MAX];
    struct group *g;
    int index = 0;

    setgrent();

    groups[index++] = basegid;

    while (index < NGROUPS_MAX && ((g = getgrent()) != NULL)) {
        if (g->gr_gid != basegid) {
            char **names;

            for (names = g->gr_mem; *names != NULL; ++names) {
                if (!strcmp(*names, name))
                    groups[index++] = g->gr_gid;
            }
        }
    }

    endgrent();

    return setgroups(index, groups);
#endif
}
#endif /* def HAVE_INITGROUPS */

/* standard mpm configuration handling */

const char *ap_mpm_set_pidfile(cmd_parms *cmd, void *dummy,
                               const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    if (cmd->server->is_virtual) {
        return "PidFile directive not allowed in <VirtualHost>";
    }

    ap_pid_fname = arg;
    return NULL;
}

const char *ap_mpm_set_max_requests(cmd_parms *cmd, void *dummy,
                                    const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    if (!strcasecmp(cmd->cmd->name, "MaxRequestsPerChild")) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL,
                     "MaxRequestsPerChild is deprecated, use "
                     "MaxConnectionsPerChild instead.");
    }

    ap_max_requests_per_child = atoi(arg);

    return NULL;
}

const char *ap_mpm_set_coredumpdir(cmd_parms *cmd, void *dummy,
                                   const char *arg)
{
    apr_finfo_t finfo;
    const char *fname;
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    fname = ap_server_root_relative(cmd->pool, arg);
    if (!fname) {
        return apr_pstrcat(cmd->pool, "Invalid CoreDumpDirectory path ",
                           arg, NULL);
    }
    if (apr_stat(&finfo, fname, APR_FINFO_TYPE, cmd->pool) != APR_SUCCESS) {
        return apr_pstrcat(cmd->pool, "CoreDumpDirectory ", fname,
                           " does not exist", NULL);
    }
    if (finfo.filetype != APR_DIR) {
        return apr_pstrcat(cmd->pool, "CoreDumpDirectory ", fname,
                           " is not a directory", NULL);
    }
    apr_cpystrn(ap_coredump_dir, fname, sizeof(ap_coredump_dir));
    ap_coredumpdir_configured = 1;
    return NULL;
}

const char * ap_mpm_set_graceful_shutdown(cmd_parms *cmd, void *dummy,
                                          const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }
    ap_graceful_shutdown_timeout = atoi(arg);
    return NULL;
}

const char *ap_mpm_set_max_mem_free(cmd_parms *cmd, void *dummy,
                                    const char *arg)
{
    long value;
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    value = strtol(arg, NULL, 0);
    if (value < 0 || errno == ERANGE)
        return apr_pstrcat(cmd->pool, "Invalid MaxMemFree value: ",
                           arg, NULL);

    ap_max_mem_free = (apr_uint32_t)value * 1024;

    return NULL;
}

const char *ap_mpm_set_thread_stacksize(cmd_parms *cmd, void *dummy,
                                        const char *arg)
{
    long value;
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    value = strtol(arg, NULL, 0);
    if (value < 0 || errno == ERANGE)
        return apr_pstrcat(cmd->pool, "Invalid ThreadStackSize value: ",
                           arg, NULL);

    ap_thread_stacksize = (apr_size_t)value;

    return NULL;
}

AP_DECLARE(apr_status_t) ap_mpm_query(int query_code, int *result)
{
    apr_status_t rv;

    if (ap_run_mpm_query(query_code, result, &rv) == DECLINED) {
        rv = APR_EGENERAL;
    }

    return rv;
}

/* core's child-status hook
 * tracks number of remaining children per generation and
 * runs the end-generation hook when a generation finishes
 */
void ap_core_child_status(server_rec *s, pid_t pid,
                          ap_generation_t gen, int slot,
                          mpm_child_status status)
{
    mpm_gen_info_t *cur;
    const char *status_msg = "unknown status";

    if (!gen_head_init) { /* where to run this? */
        gen_head_init = 1;
        APR_RING_INIT(&geninfo, mpm_gen_info_t, link);
        APR_RING_INIT(&unused_geninfo, mpm_gen_info_t, link);
    }

    cur = APR_RING_FIRST(&geninfo);
    while (cur != APR_RING_SENTINEL(&geninfo, mpm_gen_info_t, link) &&
           cur->gen != gen) {
        cur = APR_RING_NEXT(cur, link);
    }

    switch(status) {
    case MPM_CHILD_STARTED:
        status_msg = "started";
        if (cur == APR_RING_SENTINEL(&geninfo, mpm_gen_info_t, link)) {
            /* first child for this generation */
            if (!APR_RING_EMPTY(&unused_geninfo, mpm_gen_info_t, link)) {
                cur = APR_RING_FIRST(&unused_geninfo);
                APR_RING_REMOVE(cur, link);
            }
            else {
                cur = apr_pcalloc(s->process->pool, sizeof *cur);
            }
            cur->gen = gen;
            APR_RING_ELEM_INIT(cur, link);
            APR_RING_INSERT_HEAD(&geninfo, cur, mpm_gen_info_t, link);
        }
        ++cur->active;
        break;
    case MPM_CHILD_EXITED:
        status_msg = "exited";
        if (cur == APR_RING_SENTINEL(&geninfo, mpm_gen_info_t, link)) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                         "no record of generation %d of exiting child %" APR_PID_T_FMT,
                         gen, pid);
        }
        else {
            --cur->active;
            if (!cur->active) {
                ap_log_error(APLOG_MARK, APLOG_TRACE4, 0, ap_server_conf,
                             "end of generation %d", gen);
                ap_run_end_generation(ap_server_conf, gen);
                APR_RING_REMOVE(cur, link);
                APR_RING_INSERT_HEAD(&unused_geninfo, cur, mpm_gen_info_t, link);
            }
        }
        break;
    case MPM_CHILD_LOST_SLOT:
        status_msg = "lost slot";
        /* we don't track by slot, so it doesn't matter */
        break;
    }
    ap_log_error(APLOG_MARK, APLOG_TRACE4, 0, s,
                 "mpm child %" APR_PID_T_FMT " (gen %d/slot %d) %s",
                 pid, gen, slot, status_msg);
}

AP_DECLARE(apr_status_t) ap_mpm_register_timed_callback(apr_time_t t, ap_mpm_callback_fn_t *cbfn, void *baton)
{
    return ap_run_mpm_register_timed_callback(t, cbfn, baton);
}

AP_DECLARE(const char *)ap_show_mpm(void)
{
    const char *name = ap_run_mpm_get_name();

    if (!name) {
        name = "";
    }

    return name;
}

AP_DECLARE(const char *)ap_check_mpm(void)
{
    static const char *last_mpm_name = NULL;

    if (!_hooks.link_mpm || _hooks.link_mpm->nelts == 0)
        return "No MPM loaded.";
    else if (_hooks.link_mpm->nelts > 1)
        return "More than one MPM loaded.";

    if (last_mpm_name) {
        if (strcmp(last_mpm_name, ap_show_mpm())) {
            return "The MPM cannot be changed during restart.";
        }
    }
    else {
        last_mpm_name = apr_pstrdup(ap_pglobal, ap_show_mpm());
    }

    return NULL;
}
