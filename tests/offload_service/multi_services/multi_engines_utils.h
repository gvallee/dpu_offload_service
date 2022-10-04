//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef MULTI_ENGINES_UTILS_H_
#define MULTI_ENGINES_UTILS_H_

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"

#define NOTIF_TEST_ID (LAST_RESERVED_NOTIF_ID + 1)
#define NOTIF_TEST_DONE (NOTIF_TEST_ID + 2)
#define MSG_CLIENT_ENGINE1 (42)
#define MSG_CLIENT_ENGINE2 (MSG_CLIENT_ENGINE1 + 3)

static dpu_offload_status_t init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker);
    if (status != UCS_OK)
    {
        fprintf(stderr, "ucp_worker_create() failed\n");
        return DO_ERROR;
    }
    return DO_SUCCESS;
}

#endif // MULTI_ENGINES_UTILS_H_