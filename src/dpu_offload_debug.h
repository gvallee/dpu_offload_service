//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef _DPU_OFFLOAD_DEBUG_H
#define _DPU_OFFLOAD_DEBUG_H

#if !NDEBUG
#define DBG(_dbg_fmt, ...)                                                                         \
    fprintf(stdout, "[%s:l.%d:%s()] " _dbg_fmt "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__)
#else
#define DO_DBG() \
    do           \
    {            \
    } while (0)
#endif

#define ERR_MSG(_err_fmt, ...) \
    fprintf(stderr, "[%s:l.%d] ERROR: %s() failed. " _err_fmt "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__)

#define WARN_MSG(_warn_fmt, ...) \
    fprintf(stderr, "[%s:l.%d:%s()] WARN: " _warn_fmt "\n", __FILE__, __LINE__, __func__ __VA_OPT__(, ) __VA_ARGS__)


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