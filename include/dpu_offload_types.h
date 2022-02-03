//
// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdbool.h>
#include <pthread.h>

#ifndef DPU_OFFLOAD_TYPES_H
#define DPU_OFFLOAD_TYPES_H

#include <ucp/api/ucp.h>
#include <ucs/datastruct/list.h>

#include "dynamic_list.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

typedef enum
{
    DAEMON_CLIENT = 0,
    DAEMON_SERVER
} daemon_type_t;

#define DAEMON_GET_WORKER(_d, _w) ({ \
    if (_d->type == DAEMON_CLIENT)   \
    {                                \
        fprintf(stderr, "Client\n"); \
        _w = _d->client->ucp_worker; \
    }                                \
    else                             \
    {                                \
        fprintf(stderr, "Server\n"); \
        _w = _d->server->ucp_worker; \
    }                                \
})

typedef enum dpu_offload_state {
    DPU_OFFLOAD_STATE_UNKNOWN = 0,
    DPU_OFFLOAD_STATE_INITIALIZED,
    DPU_OFFLOAD_STATE_FINALIZED,
} dpu_offload_state_t;

typedef struct pmix_infrastructure {
    int dvm_argc;
    char **dvm_argv;
    int run_argc;
    char **run_argv;
    pid_t dvm_pid;
    pid_t service_pid;
    bool dvm_started;
    bool service_started;
} pmix_infrastructure_t;

typedef struct offload_config {
    dpu_offload_state_t state;
    char *offload_config_file_path;
    char *associated_bluefield;
    bool with_pmix;

    union {
        pmix_infrastructure_t pmix;
    } infra;
} offload_config_t;

typedef struct ucx_server_ctx
{
    volatile ucp_conn_request_h conn_request;
    ucp_listener_h listener;
} ucx_server_ctx_t;

typedef struct dpu_offload_server_t
{
    int mode;
    char *ip_str;
    char *port_str;
    uint16_t port;
    struct sockaddr_storage saddr;
    ucp_worker_h ucp_worker;
    ucp_context_h ucp_context;
    ucp_ep_h client_ep;
    ucs_status_t client_ep_status;
    union
    {
        struct
        {
            ucx_server_ctx_t context;
        } ucx_listener;
        struct
        {
            ucp_address_t *local_addr;
            size_t local_addr_len;
            void *peer_addr;
            size_t peer_addr_len;
            int sock;
            int tag;
            char *addr_msg_str;
            ucp_tag_t tag_mask;
        } oob;
    } conn_data;
} dpu_offload_server_t;

typedef struct dpu_offload_client_t
{
    int mode;
    char *address_str;
    char *port_str;
    uint16_t port;

    ucp_worker_h ucp_worker;
    ucp_context_h ucp_context;
    ucp_ep_h server_ep;
    ucs_status_t server_ep_status;
    union
    {
        struct
        {
            struct sockaddr_storage connect_addr;
        } ucx_listener;
        struct
        {
            ucp_address_t *local_addr;
            size_t local_addr_len;
            void *peer_addr;
            size_t peer_addr_len;
            int sock;
            char *addr_msg_str;
            int tag;
        } oob;
    } conn_data;
} dpu_offload_client_t;

typedef struct am_req_t
{
    uint64_t hdr;
    int complete;
} am_req_t;

typedef struct dpu_offload_event
{
    ucs_list_link_t item;
    uint64_t id;
    am_req_t ctx;
    void *req;
    void *context;
    void *data;
} dpu_offload_event_t;

typedef int (*notification_cb)(void *context, void *data);

typedef struct notification_callback_entry {
    bool set;
    notification_cb cb;
} notification_callback_entry_t;

typedef struct pending_notification {
    ucs_list_link_t item;
    uint64_t type;
    void *header;
    size_t header_size;
    void *data;
    size_t data_size;
    void *arg;
} pending_notification_t;

typedef struct dpu_offload_ev_sys
{
    dyn_list_t *free_evs;
    size_t num_used_evs;
    uint64_t num_notification_callbacks;
    ucs_list_link_t pending_notifications;
    dyn_list_t *free_pending_notifications;

    // Array of callback functions, i.e., array of pointers
    notification_callback_entry_t *notification_callbacks;
} dpu_offload_ev_sys_t;

typedef enum
{
    EVENT_DONE = UCS_OK,
    EVENT_INPROGRESS = UCS_INPROGRESS
} event_state_t;

typedef struct
{
    uint8_t type;
    int done;
    dpu_offload_ev_sys_t *event_channels;
    union
    {
        dpu_offload_client_t *client;
        dpu_offload_server_t *server;
    };
} dpu_offload_daemon_t;

typedef enum
{
    AM_TERM_MSG_ID = 33,
    AM_EVENT_MSG_ID,
    AM_TEST_MSG_ID
} am_id_t;

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif // DPU_OFFLOAD_TYPES_H