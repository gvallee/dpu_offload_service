//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef DPU_OFFLOAD_SERVICE_DAEMON_H_
#define DPU_OFFLOAD_SERVICE_DAEMON_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <string.h>

#include <ucp/api/ucp.h>

typedef struct ucx_server_ctx
{
    volatile ucp_conn_request_h conn_request;
    ucp_listener_h listener;
} ucx_server_ctx_t;

typedef struct dpu_offload_server_t
{
    int done;
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
    int done;
    int mode;
    char *address_str;
    char *port_str;
    uint16_t port;

    ucp_worker_h ucp_worker;
    ucp_context_h ucp_context;
    //ucp_ep_h ep;
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
    int complete;
} am_req_t;

struct ucx_context
{
    int completed;
};

typedef enum {
    FAILURE_MODE_NONE,
    FAILURE_MODE_SEND,      /* fail send operation on server */
    FAILURE_MODE_RECV,      /* fail receive operation on client */
    FAILURE_MODE_KEEPALIVE  /* fail without communication on client */
} failure_mode_t;

static struct err_handling
{
    ucp_err_handling_mode_t ucp_err_mode;
    failure_mode_t failure_mode;
} err_handling_opt;

enum
{
    OOB,
    UCX_LISTENER
} conn_mode_t;

enum
{
    AM_TERM_MSG_ID = 0,
    AM_TEST_MSG_ID
} am_id_t;

int server_init(dpu_offload_server_t **server);
int server_progress(dpu_offload_server_t *server);
void server_fini(dpu_offload_server_t **server);

int client_init(dpu_offload_client_t **client);
void client_fini(dpu_offload_client_t **client);

#endif // DPU_OFFLOAD_SERVICE_DAEMON_H_