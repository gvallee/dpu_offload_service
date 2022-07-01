//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _DPU_OFFLOAD_DEBUG_H
#define _DPU_OFFLOAD_DEBUG_H

#include <sys/types.h>
#include <unistd.h>

#include "dpu_offload_envvars.h"

#if !NDEBUG
typedef struct debug_config
{
    char *my_hostname;
    int verbose;
    char *sp_id_str;
    int sp_id; // Only relevent on DPUs
} debug_config_t;
extern debug_config_t dbg_cfg;
#define DBG(_dbg_fmt, ...)                                                            \
    do                                                                                \
    {                                                                                 \
        if (dbg_cfg.my_hostname == NULL)                                              \
        {                                                                             \
            dbg_cfg.my_hostname = malloc(1024);                                       \
            dbg_cfg.my_hostname[1023] = '\0';                                         \
            gethostname(dbg_cfg.my_hostname, 1023);                                   \
            dbg_cfg.verbose = 0;                                                      \
            char *verbose_str = getenv(DPU_OFFLOAD_DBG_VERBOSE);                      \
            if (verbose_str)                                                          \
                dbg_cfg.verbose = atoi(verbose_str);                                  \
            dbg_cfg.sp_id_str = getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR); \
            if (dbg_cfg.sp_id_str != NULL)                                            \
                dbg_cfg.sp_id = atoi(dbg_cfg.sp_id_str);                              \
        }                                                                             \
        if (dbg_cfg.verbose > 0)                                                      \
        {                                                                             \
            if (dbg_cfg.sp_id_str == NULL)                                            \
                fprintf(stdout, "[%s:l.%d:%s():%s:pid=%d] " _dbg_fmt "\n",            \
                        __FILE__, __LINE__, __func__, dbg_cfg.my_hostname,            \
                        getpid() __VA_OPT__(, ) __VA_ARGS__);                         \
            else                                                                      \
                fprintf(stdout, "[%s:l.%d:%s():%s:pid=%d,SP=%d] " _dbg_fmt "\n",      \
                        __FILE__, __LINE__, __func__, dbg_cfg.my_hostname,            \
                        getpid(), dbg_cfg.sp_id __VA_OPT__(, ) __VA_ARGS__);          \
        }                                                                             \
    } while (0)
#else
#define DBG(...) \
    do           \
    {            \
    } while (0)
#endif

#define ERR_MSG(_err_fmt, ...)                                                              \
    do                                                                                      \
    {                                                                                       \
        char myhostname[1024];                                                              \
        myhostname[1023] = '\0';                                                            \
        gethostname(myhostname, 1023);                                                      \
        if (getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR))                           \
        {                                                                                   \
            int sp = atoi(getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR));            \
            fprintf(stderr, "[%s:l.%d:%s:pid=%d:SP=%d] ERROR: %s() failed. " _err_fmt "\n", \
                    __FILE__, __LINE__, myhostname, getpid(), sp,                           \
                    __func__ __VA_OPT__(, ) __VA_ARGS__);                                   \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            fprintf(stderr, "[%s:l.%d:%s:pid=%d] ERROR: %s() failed. " _err_fmt "\n",       \
                    __FILE__, __LINE__, myhostname, getpid(),                               \
                    __func__ __VA_OPT__(, ) __VA_ARGS__);                                   \
        }                                                                                   \
    } while (0)

#define WARN_MSG(_warn_fmt, ...)                                                    \
    do                                                                              \
    {                                                                               \
        char myhostname[1024];                                                      \
        myhostname[1023] = '\0';                                                    \
        gethostname(myhostname, 1023);                                              \
        if (getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR))                   \
        {                                                                           \
            int sp = atoi(getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR));    \
            fprintf(stderr, "[%s:l.%d:%s():%s:pid=%d:SP=%d] WARN: " _warn_fmt "\n", \
                    __FILE__, __LINE__, __func__, myhostname,                       \
                    getpid(), sp __VA_OPT__(, ) __VA_ARGS__);                       \
        }                                                                           \
        else                                                                        \
        {                                                                           \
            fprintf(stderr, "[%s:l.%d:%s():%s:pid=%d] WARN: " _warn_fmt "\n",       \
                    __FILE__, __LINE__, __func__, myhostname,                       \
                    getpid() __VA_OPT__(, ) __VA_ARGS__);                           \
        }                                                                           \
    } while (0)

#define INFO_MSG(_info_fmt, ...)                                                    \
    do                                                                              \
    {                                                                               \
        char myhostname[1024];                                                      \
        myhostname[1023] = '\0';                                                    \
        gethostname(myhostname, 1023);                                              \
        if (getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR))                   \
        {                                                                           \
            int sp = atoi(getenv(DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID_ENVVAR));    \
            fprintf(stderr, "[%s:l.%d:%s():%s:pid=%d:SP=%d] INFO: " _info_fmt "\n", \
                    __FILE__, __LINE__, __func__, myhostname,                       \
                    getpid(), sp __VA_OPT__(, ) __VA_ARGS__);                       \
        }                                                                           \
        else                                                                        \
        {                                                                           \
            fprintf(stderr, "[%s:l.%d:%s():%s:pid=%d] INFO: " _info_fmt "\n",       \
                    __FILE__, __LINE__, __func__, myhostname,                       \
                    getpid() __VA_OPT__(, ) __VA_ARGS__);                           \
        }                                                                           \
    } while (0)

#if !NDEBUG
#define CHECK_ERR_GOTO(_exp, _label, _check_fmt, ...) \
    do                                                \
    {                                                 \
        if (_exp)                                     \
        {                                             \
            ERR_MSG(_check_fmt, __VA_ARGS__);         \
            goto _label;                              \
        }                                             \
    } while (0)
#else
#define CHECK_ERR_GOTO(...) \
    do                      \
    {                       \
    } while (0)
#endif

#if !NDEBUG
#define CHECK_ERR_RETURN(_exp, _rc, _check_fmt, ...) \
    do                                               \
    {                                                \
        if (_exp)                                    \
        {                                            \
            ERR_MSG(_check_fmt, __VA_ARGS__);        \
            return _rc;                              \
        }                                            \
    } while (0)
#else
#define CHECK_ERR_RETURN(...) \
    do                        \
    {                         \
    } while (0)
#endif

#endif // _DPU_OFFLOAD_DEBUG_H
