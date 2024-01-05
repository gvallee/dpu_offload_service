#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_envvars.h"
#include "dynamic_structs.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_ops.h"
#include "dpu_offload_comms.h"

static dpu_offload_status_t execution_context_init(offloading_engine_t *offload_engine, uint64_t type, execution_context_t **econtext);
static void execution_context_fini(execution_context_t **ctx);

extern dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *);
#if USE_AM_IMPLEM
extern int am_send_event_msg(dpu_offload_event_t **event);
#else
static void progress_self_event_recv(execution_context_t *econtext);
extern int tag_send_event_msg(dpu_offload_event_t **event);
#endif

// A lot of the code is from ucp_client_serrver.c from UCX

static sa_family_t ai_family = AF_INET;

#define DEFAULT_MAX_NUM_CLIENTS (256)
#define DEFAULT_MAX_NUM_SERVERS (256)

#define IP_STRING_LEN 50
#define PORT_STRING_LEN 8
#define OOB_DEFAULT_TAG 0x1388u
#define UCX_ADDR_MSG "UCX address message"

static struct err_handling
{
    ucp_err_handling_mode_t ucp_err_mode;
    failure_mode_t failure_mode;
} err_handling_opt;

struct oob_msg
{
    uint64_t len;
};

#define ADD_DEFAULT_ENGINE_CALLBACKS(_engine, _econtext)                                                                        \
    do                                                                                                                          \
    {                                                                                                                           \
        if ((_engine)->num_default_notifications > 0)                                                                           \
        {                                                                                                                       \
            notification_callback_entry_t *_list_callbacks;                                                                     \
            _list_callbacks = (notification_callback_entry_t *)(_engine)->default_notifications->notification_callbacks.base;   \
            size_t _i;                                                                                                          \
            size_t _n = 0;                                                                                                      \
            for (_i = 0; _i < (_engine)->default_notifications->notification_callbacks.capacity; _i++)                          \
            {                                                                                                                   \
                if (_list_callbacks[_i].set)                                                                                    \
                {                                                                                                               \
                    if (_list_callbacks[_i].info.mem_pool == NULL)                                                              \
                    {                                                                                                           \
                        dpu_offload_status_t _rc = event_channel_register((_econtext)->event_channels,                          \
                                                                          _i,                                                   \
                                                                          _list_callbacks[_i].cb,                               \
                                                                          NULL);                                                \
                        CHECK_ERR_GOTO((_rc), error_out,                                                                        \
                                       "unable to register engine's default notification to new execution context (type: %ld)", \
                                       _i);                                                                                     \
                    }                                                                                                           \
                    else                                                                                                        \
                    {                                                                                                           \
                        dpu_offload_status_t _rc = event_channel_register((_econtext)->event_channels,                          \
                                                                          _i,                                                   \
                                                                          _list_callbacks[_i].cb,                               \
                                                                          &(_list_callbacks[_i].info));                         \
                        CHECK_ERR_GOTO((_rc), error_out,                                                                        \
                                       "unable to register engine's default notification to new execution context (type: %ld)", \
                                       _i);                                                                                     \
                    }                                                                                                           \
                    _n++;                                                                                                       \
                }                                                                                                               \
                if (_n == (_engine)->num_default_notifications)                                                                 \
                {                                                                                                               \
                    /* All done, no need to continue parsing the array. */                                                      \
                    break;                                                                                                      \
                }                                                                                                               \
            }                                                                                                                   \
        }                                                                                                                       \
    } while (0)

extern dpu_offload_status_t get_env_config(offloading_engine_t *engine, conn_params_t *params);

static void oob_recv_generic_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    DBG("Bootstrapping message received (ctx: %p)", user_data);
    am_req_t *ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

/**
 * @brief Note: this function assumes the execution context is NOT locked before it is invoked.
 * This function is meant to be called by the main thread, NOT the connection thread.
 *
 * @param econtext
 * @param client_addr
 * @return dpu_offload_status_t
 */
static inline dpu_offload_status_t oob_server_ucx_client_connection_step1(execution_context_t *econtext, peer_info_t *client_info)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");
    ECONTEXT_LOCK(econtext);

    dpu_offload_server_t *server = econtext->server;
    CHECK_ERR_GOTO((server == NULL), error_out, "server handle is undefined");

    /* 1. Receive the address size */
    ucp_tag_t ucp_tag, ucp_tag_mask;
    DBG("Posting recv - Tag: %d, scope_id: %d, econtext: %p, peer_id: %ld",
        econtext->server->conn_data.oob.tag,
        econtext->scope_id,
        econtext, client_info->id);
    MAKE_RECV_TAG(ucp_tag,
                  ucp_tag_mask,
                  econtext->server->conn_data.oob.tag,
                  client_info->id,
                  econtext->server->id,
                  econtext->scope_id,
                  0);
    ucp_request_param_t recv_param = {0};
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.addr_size_ctx);
    recv_param.cb.recv = oob_recv_generic_handler;
    assert(client_info->bootstrapping.addr_size_ctx.complete == false);
    client_info->bootstrapping.addr_size_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                                    &(client_info->peer_addr_len),
                                                                    sizeof(size_t),
                                                                    ucp_tag,
                                                                    ucp_tag_mask,
                                                                    &recv_param);
    if (UCS_PTR_IS_ERR(client_info->bootstrapping.addr_size_request))
    {
        ERR_MSG("ucp_tag_recv_nbx() failed");
        goto error_out;
    }
    if (client_info->bootstrapping.addr_size_request == NULL)
    {
        DBG("We received the address size right away, callback not invoked, dealing directly with it");
        client_info->bootstrapping.addr_size_ctx.complete = true;
    }
    else
    {
        DBG("Reception of the address size still in progress (engine: %p)", econtext->engine);
        assert(client_info->bootstrapping.addr_size_ctx.complete == false);
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

static inline dpu_offload_status_t oob_server_ucx_client_connection_step2(execution_context_t *econtext, peer_info_t *client_info)
{
    // request_finalize(GET_WORKER(econtext), addr_size_request, &ctx);
    DBG("Client address size: %ld", client_info->peer_addr_len);
    assert(client_info->peer_addr_len > 0);
    ECONTEXT_LOCK(econtext);

    /* 2. Receive the address itself */

    // Get the buffer ready for the receive
    ucp_tag_t ucp_tag, ucp_tag_mask;
    ucp_request_param_t recv_param = {0};
    client_info->peer_addr = DPU_OFFLOAD_MALLOC(client_info->peer_addr_len);
    CHECK_ERR_GOTO((client_info->peer_addr == NULL),
                   error_out,
                   "unable to allocate memory for peer address (%ld bytes)",
                   client_info->peer_addr_len);
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.addr_ctx);
    recv_param.cb.recv = oob_recv_generic_handler;
    DBG("Tag: %d; scope_id: %d\n", econtext->server->conn_data.oob.tag, econtext->scope_id);
    MAKE_RECV_TAG(ucp_tag,
                  ucp_tag_mask,
                  econtext->server->conn_data.oob.tag,
                  client_info->id,
                  econtext->server->id,
                  econtext->scope_id,
                  0);
    client_info->bootstrapping.addr_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                               client_info->peer_addr,
                                                               client_info->peer_addr_len,
                                                               ucp_tag,
                                                               ucp_tag_mask,
                                                               &recv_param);
    if (UCS_PTR_IS_ERR(client_info->bootstrapping.addr_request))
    {
        ERR_MSG("ucp_tag_recv_nbx() failed");
        goto error_out;
    }
    DBG("Recv for client's address successfully posted");
    if (client_info->bootstrapping.addr_request == NULL)
    {
        DBG("We received the address right away, callback not invoked, dealing directly with it");
        client_info->bootstrapping.addr_ctx.complete = true;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

static inline dpu_offload_status_t oob_server_ucx_client_connection_step3(execution_context_t *econtext, peer_info_t *client_info)
{
    /* 3. Create the endpoint using the address we just received */
    assert(econtext->type > 0); // should not be unknown
    assert(econtext->type < CONTEXT_LIMIT_MAX);
    ucp_ep_params_t ep_params = {0};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.arg = econtext;
    ep_params.address = client_info->peer_addr;
    ep_params.user_data = &(client_info->ep_status);
    ucp_worker_h worker = GET_WORKER(econtext);
    CHECK_ERR_GOTO((worker == NULL), error_out, "undefined worker");
    ucp_ep_h client_ep;
    ucs_status_t status = ucp_ep_create(worker, &ep_params, &client_ep);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "ucp_ep_create() failed: %s", ucs_status_string(status));
    client_info->ep = client_ep;
    DBG("endpoint %p successfully created", client_ep);

    // receive the rank/group data
    ucp_tag_t ucp_tag, ucp_tag_mask;
    ucp_request_param_t recv_param = {0};
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.rank_ctx);
    recv_param.cb.recv = oob_recv_generic_handler;
    MAKE_RECV_TAG(ucp_tag,
                  ucp_tag_mask,
                  econtext->server->conn_data.oob.tag,
                  client_info->id,
                  econtext->server->id,
                  econtext->scope_id,
                  0);
    client_info->bootstrapping.rank_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                               &(client_info->rank_data),
                                                               sizeof(rank_info_t),
                                                               ucp_tag,
                                                               ucp_tag_mask,
                                                               &recv_param);
    if (UCS_PTR_IS_ERR(client_info->bootstrapping.rank_request))
    {
        ucs_status_t send_status = UCS_PTR_STATUS(client_info->bootstrapping.rank_request);
        ERR_MSG("ucp_tag_recv_nbx() failed: %s", ucs_status_string(send_status));
        goto error_out;
    }
    DBG("Receiving rank info (len=%ld, req=%p)", sizeof(rank_info_t), client_info->bootstrapping.rank_request);
    if (client_info->bootstrapping.rank_request == NULL)
    {
        DBG("We received the rank info right away, callback not invoked, dealing directly with it");
        client_info->bootstrapping.rank_ctx.complete = true;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

static void ucx_client_bootstrap_send_cb(void *request, ucs_status_t status, void *user_data)
{
    am_req_t *ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

// Optional step 4 of a client-server connection, used in the context of the server. It is used ONLY
// when a client from a host connects to a SP and is used by the SP to send data about itself that
// the client cannot set on its own.
static inline dpu_offload_status_t oob_server_ucx_client_connection_step4(execution_context_t *econtext, peer_info_t *client_info)
{
    ucp_tag_t ucp_tag;
    ucp_request_param_t send_param = {0};

    assert(econtext);
    assert(client_info);
    assert(client_info->ep);

    ECONTEXT_LOCK(econtext);
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_bootstrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(client_info->bootstrapping.step4.ctx);

    ucp_tag = MAKE_SEND_TAG(econtext->server->conn_data.oob.tag,
                            client_info->id,
                            econtext->server->id,
                            econtext->scope_id,
                            0);
    client_info->bootstrapping.step4.request = ucp_tag_send_nbx(client_info->ep,
                                                                &(econtext->engine->config->local_service_proc.info),
                                                                sizeof(service_proc_t),
                                                                ucp_tag,
                                                                &send_param);
    if (UCS_PTR_IS_ERR(client_info->bootstrapping.step4.request))
    {
        ERR_MSG("ucp_tag_send_nbx() failed");
        return DO_ERROR;
    }
    if (client_info->bootstrapping.step4.request == NULL)
    {
        DBG("Send of SP info completed right away");
        client_info->bootstrapping.step4.ctx.complete = true;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
}

dpu_offload_status_t set_sock_addr(char *addr, uint16_t port, struct sockaddr_storage *saddr)
{
    struct sockaddr_in *sa_in;
    struct sockaddr_in6 *sa_in6;

    /* The server will listen on INADDR_ANY */
    memset(saddr, 0, sizeof(*saddr));
    switch (ai_family)
    {
    case AF_INET:
        sa_in = (struct sockaddr_in *)saddr;
        if (addr != NULL)
        {
            int rc = inet_pton(AF_INET, addr, &sa_in->sin_addr);
            if (rc <= 0)
            {
                if (rc == 0)
                    ERR_MSG("Not in presentation format\n");
                else
                    ERR_MSG("inet_pton() failed\n");
            }
        }
        else
        {
            sa_in->sin_addr.s_addr = INADDR_ANY;
        }
        sa_in->sin_family = AF_INET;
        sa_in->sin_port = htons(port);
        break;
    case AF_INET6:
        sa_in6 = (struct sockaddr_in6 *)saddr;
        if (addr != NULL)
        {
            inet_pton(AF_INET6, addr, &sa_in6->sin6_addr);
        }
        else
        {
            sa_in6->sin6_addr = in6addr_any;
        }
        sa_in6->sin6_family = AF_INET6;
        sa_in6->sin6_port = htons(port);
        break;
    default:
        ERR_MSG("Invalid address family");
        break;
    }

    return DO_SUCCESS;
}

/**
 * @brief Note: the function assumes the econtext is NOT locked before it is invoked
 *
 * @param econtext
 * @param client_addr
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t oob_server_accept(execution_context_t *econtext, char *client_addr)
{
    struct sockaddr_in servaddr;
    int optval = 1;
    int rc;
    int accept_sock;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(client_addr);

    ECONTEXT_LOCK(econtext);
    uint16_t server_port = econtext->server->conn_params.port;

    if (econtext->server->conn_data.oob.listenfd == -1)
    {
        econtext->server->conn_data.oob.listenfd = socket(AF_INET, SOCK_STREAM, 0);

        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port = htons(server_port);

        setsockopt(econtext->server->conn_data.oob.listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        rc = bind(econtext->server->conn_data.oob.listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
        CHECK_ERR_GOTO((rc), error_out, "bind() failed on port %d: %s", server_port, strerror(errno));
        rc = listen(econtext->server->conn_data.oob.listenfd, 1024);
        CHECK_ERR_GOTO((rc), error_out, "listen() failed: %s", strerror(errno));

        // fixme: debug when multi-connections are required
        DBG("Listening for connections on port %" PRIu16 "... (server: %" PRIu64 ", econtext: %p, scope_id=%d)",
            server_port, econtext->server->id, econtext, econtext->scope_id);
    }
    else
    {
        DBG("Already listening on socket %d", econtext->server->conn_data.oob.listenfd);
    }
    ECONTEXT_UNLOCK(econtext);
    accept_sock = accept(econtext->server->conn_data.oob.listenfd, (struct sockaddr *)&addr, &addr_len);
    ECONTEXT_LOCK(econtext);
    CHECK_ERR_GOTO((accept_sock == -1), error_out, "accept failed: %s", strerror(errno));
    econtext->server->conn_data.oob.sock = accept_sock;
    struct in_addr ipAddr = addr.sin_addr;
    inet_ntop(AF_INET, &ipAddr, client_addr, INET_ADDRSTRLEN);
    DBG("Connection accepted from %s on fd=%d", client_addr, econtext->server->conn_data.oob.sock);
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;

error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

// This function assumes the associated execution context is properly locked before it is invoked
dpu_offload_status_t oob_client_connect(dpu_offload_client_t *client, sa_family_t af)
{
    char service[8];
    struct addrinfo hints, *res, *t;
    int ret;
    int rc = 0;

    snprintf(service, sizeof(service), "%u", client->conn_params.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = (client->conn_params.addr_str == NULL) ? AI_PASSIVE : 0;
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;

    DBG("Connecting to %s:%" PRIu16, client->conn_params.addr_str, client->conn_params.port);

    ret = getaddrinfo(client->conn_params.addr_str, service, &hints, &res);
    CHECK_ERR_RETURN((ret < 0), DO_ERROR, "getaddrinfo() failed");

    for (t = res; t != NULL; t = t->ai_next)
    {
        client->conn_data.oob.sock = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (client->conn_data.oob.sock < 0)
        {
            continue;
        }

        DBG("Connecting to server...");
        do
        {
            rc = connect(client->conn_data.oob.sock, t->ai_addr, t->ai_addrlen);
            if (rc == 0)
            {
                struct sockaddr_in conn_addr;
                socklen_t conn_addr_len = sizeof(conn_addr);
                memset(&conn_addr, 0, sizeof(conn_addr));
                getsockname(client->conn_data.oob.sock, (struct sockaddr *)&conn_addr, &conn_addr_len);
                char conn_ip[16];
                inet_ntop(AF_INET, &(conn_addr.sin_addr), conn_ip, sizeof(conn_ip));
                DBG("Connection established, fd = %d, addr=%s:%d", client->conn_data.oob.sock, conn_ip, ntohs(conn_addr.sin_port));
                break;
            }
        } while (rc != 0);
        CHECK_ERR_GOTO((rc != 0), err_close_sockfd, "Connection failed (rc: %d)", rc);
    }

    CHECK_ERR_GOTO((client->conn_data.oob.sock < 0), err_close_sockfd, "Unable to connect to server: invalid file descriptor (%d)", client->conn_data.oob.sock);

out_free_res:
    freeaddrinfo(res);
    return rc;
err_close_sockfd:
    close(client->conn_data.oob.sock);
    rc = DO_ERROR;
    goto out_free_res;
}

static void ep_close(ucp_worker_h ucp_worker, ucp_ep_h ep)
{
    ucp_request_param_t param = {0};
    ucs_status_t status;
    void *close_req;
    param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    param.flags = UCP_EP_CLOSE_FLAG_FORCE;
    close_req = ucp_ep_close_nbx(ep, &param);
    if (UCS_PTR_IS_PTR(close_req))
    {
        do
        {
            ucp_worker_progress(ucp_worker);
            status = ucp_request_check_status(close_req);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(close_req);
    }
    else if (UCS_PTR_STATUS(close_req) != UCS_OK)
    {
        ERR_MSG("failed to close ep %p", (void *)ep);
    }
}

static dpu_offload_status_t init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
{
    int ret = INIT_WORKER(*ucp_context, ucp_worker);
    CHECK_ERR_GOTO((ret != 0), err_cleanup, "INIT_WORKER() failed");
    DBG("ucp worker successfully created: %p", *ucp_worker);
    return DO_SUCCESS;
err_cleanup:
    ucp_cleanup(*ucp_context);
    return DO_ERROR;
}

static void common_cb(void *user_data, const char *type_str)
{
    am_req_t *ctx;
    if (user_data == NULL)
    {
        ERR_MSG("user_data passed to %s mustn't be NULL", type_str);
        return;
    }
    ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

static void send_cb(void *request, ucs_status_t status, void *user_data)
{
    common_cb(user_data, "send_cb");
}

#define __DO_INIT_WORKER(_econtext, _init_params) ({                                       \
    ucp_worker_h _econtext_worker;                                                         \
    bool _worker_set = true;                                                               \
    if ((_init_params) == NULL || (_init_params)->worker == NULL)                          \
    {                                                                                      \
        if ((_econtext)->engine->ucp_context == NULL)                                      \
        {                                                                                  \
            (_econtext)->engine->ucp_context = INIT_UCX();                                 \
            (_econtext)->engine->ucp_context_allocated = true;                             \
        }                                                                                  \
        if ((_econtext)->engine->ucp_worker == NULL)                                       \
        {                                                                                  \
            dpu_offload_status_t _ret = init_context(&((_econtext)->engine->ucp_context),  \
                                                     &(_econtext_worker));                 \
            CHECK_ERR_RETURN((_ret != 0), DO_ERROR, "init_context() failed");              \
            SET_WORKER((_econtext), _econtext_worker);                                     \
            (_econtext)->engine->ucp_worker_allocated = true;                              \
            _worker_set = true;                                                            \
            DBG("context successfully initialized (worker=%p)", _econtext_worker);         \
        }                                                                                  \
    }                                                                                      \
    if ((_init_params) != NULL && (_init_params)->worker != NULL)                          \
    {                                                                                      \
        DBG("re-using UCP worker %p", (_init_params)->worker);                             \
        /* Notes: the context usually imposes different requirements:                   */ \
        /* - on DPUs, we know there is no external UCX context and worker so they are   */ \
        /*   initialized early on, during the initialization of the engine so we can    */ \
        /*   establish connections to other service processes and set                   */ \
        /*   the self endpoint that is required to maintain the endpoint cache.         */ \
        /* - on the host, it depends on the calling code: if the UCX context and worker */ \
        /*   are available when the engine is initialized, they can be passed in then;  */ \
        /*   but in other cases like UCC, they are not available at the time and set    */ \
        /*   during the creation of the execution contexts.                             */ \
        if ((_econtext)->engine->ucp_context == NULL)                                      \
        {                                                                                  \
            DBG("UCP context for engine %p is now %p",                                     \
                (_econtext)->engine, (_init_params)->ucp_context);                         \
            (_econtext)->engine->ucp_context = (_init_params)->ucp_context;                \
        }                                                                                  \
        if (GET_WORKER(_econtext) == NULL)                                                 \
        {                                                                                  \
            SET_WORKER(_econtext, (_init_params)->worker);                                 \
            _worker_set = true;                                                            \
        }                                                                                  \
        else                                                                               \
        {                                                                                  \
            assert((_econtext)->engine->ucp_worker == (_init_params)->worker);             \
        }                                                                                  \
    }                                                                                      \
    _worker_set;                                                                           \
})

#if USE_AM_IMPLEM
#define DO_INIT_WORKER(_econtext, _init_params)    \
    do                                             \
    {                                              \
        __DO_INIT_WORKER(_econtext, _init_params); \
    } while (0)
#else
#define DO_INIT_WORKER(_econtext, _init_params)                                                   \
    do                                                                                            \
    {                                                                                             \
        bool _worker_set = __DO_INIT_WORKER(_econtext, _init_params);                             \
        if (_worker_set == true)                                                                  \
        {                                                                                         \
            /* As soon as the worker is set, we can finish to initialize notifications to self */ \
            progress_self_event_recv((_econtext)->engine->self_econtext);                         \
        }                                                                                         \
    } while (0)
#endif // USE_AM_IMPLEM

dpu_offload_status_t client_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    dpu_offload_status_t rc;
    econtext->type = CONTEXT_CLIENT;
    econtext->client = DPU_OFFLOAD_MALLOC(sizeof(dpu_offload_client_t));
    CHECK_ERR_RETURN((econtext->client == NULL), DO_ERROR, "Unable to allocate client handle\n");
    RESET_CLIENT(econtext->client);
    econtext->client->econtext = (struct execution_context *)econtext;
#if OFFLOADING_MT_ENABLE
    econtext->client->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
#endif

    // If the connection parameters were not passed in, we get everything using environment variables
    if (init_params == NULL || init_params->conn_params == NULL)
    {
        rc = get_env_config(econtext->engine, &(econtext->client->conn_params));
        CHECK_ERR_RETURN((rc), DO_ERROR, "get_env_config() failed");
    }
    else
    {
        CHECK_ERR_RETURN((init_params->conn_params->addr_str == NULL), DO_ERROR, "undefined address");
        econtext->client->conn_params.addr_str = init_params->conn_params->addr_str;
        econtext->client->conn_params.port_str = init_params->conn_params->port_str;
        econtext->client->conn_params.port = init_params->conn_params->port;
        if (init_params->id_set)
            econtext->client->id = init_params->id;
    }

    // Make sure we correctly handle whether the UCX context/worker is provided
    DO_INIT_WORKER(econtext, init_params);
    assert(GET_WORKER(econtext));

    // If we have a group/rank in the init params, we pass it down
    econtext->rank.host_info = HASH_LOCAL_HOSTNAME();
    if (init_params != NULL && init_params->proc_info != NULL)
    {
        econtext->rank.group_uid = init_params->proc_info->group_uid;
        econtext->rank.group_rank = init_params->proc_info->group_rank;
    }

    if (init_params != NULL && init_params->connected_cb != NULL)
    {
        econtext->client->connected_cb = init_params->connected_cb;
    }

    switch (econtext->client->mode)
    {
    case UCX_LISTENER:
        break;
    default:
    {
        // OOB
        econtext->client->conn_data.oob.addr_msg_str = strdup(UCX_ADDR_MSG);
        econtext->client->conn_data.oob.tag = OOB_DEFAULT_TAG;
        econtext->client->conn_data.oob.local_addr = NULL;
        econtext->client->conn_data.oob.peer_addr = NULL;
        econtext->client->conn_data.oob.local_addr_len = 0;
        econtext->client->conn_data.oob.peer_addr_len = 0;
        econtext->client->conn_data.oob.sock = -1;

        ucs_status_t status = ucp_worker_get_address(GET_WORKER(econtext),
                                                     &(econtext->client->conn_data.oob.local_addr),
                                                     &(econtext->client->conn_data.oob.local_addr_len));
        CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_worker_get_address() failed");
    }
    }

    return DO_SUCCESS;
}

// This function assumes the client structure is properly lock before it is invoked
static dpu_offload_status_t ucx_listener_client_connect(dpu_offload_client_t *client)
{
    assert(client->econtext->type > 0); // should not be unknown
    assert(client->econtext->type < CONTEXT_LIMIT_MAX);
    set_sock_addr(client->conn_params.addr_str, client->conn_params.port, &(client->conn_data.ucx_listener.connect_addr));

    /*
     * Endpoint field mask bits:
     * UCP_EP_PARAM_FIELD_FLAGS             - Use the value of the 'flags' field.
     * UCP_EP_PARAM_FIELD_SOCK_ADDR         - Use a remote sockaddr to connect
     *                                        to the remote peer.
     * UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE - Error handling mode - this flag
     *                                        is temporarily required since the
     *                                        endpoint will be closed with
     *                                        UCP_EP_CLOSE_MODE_FORCE which
     *                                        requires this mode.
     *                                        Once UCP_EP_CLOSE_MODE_FORCE is
     *                                        removed, the error handling mode
     *                                        will be removed.
     */
    ucp_ep_params_t ep_params = {0};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS |
                           UCP_EP_PARAM_FIELD_SOCK_ADDR |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode = UCP_ERR_HANDLING_MODE_NONE;
    ep_params.err_handler.arg = client->econtext;
    ep_params.flags = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    ep_params.sockaddr.addr = (struct sockaddr *)&(client->conn_data.ucx_listener.connect_addr);
    ep_params.sockaddr.addrlen = sizeof(client->conn_data.ucx_listener.connect_addr);
    DBG("Connecting to %s:%s", client->conn_params.addr_str, client->conn_params.port_str);
    execution_context_t *econtext = (execution_context_t *)client->econtext;
    ucs_status_t status = ucp_ep_create(GET_WORKER(econtext), &ep_params, &(client->server_ep));
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "failed to connect to %s (%s)", client->conn_params.addr_str,
                     ucs_status_string(status));
    DBG("Endpoint %p successfully created", client->server_ep);
    return DO_SUCCESS;
}

#if 0
static void oob_send_cb(void *request, ucs_status_t status, void *ctx)
{
    struct ucx_context *context = (struct ucx_context *)request;

    context->completed = 1;

    DBG("send handler called for \"%s\" with status %d (%s)", (const char *)ctx, status, ucs_status_string(status));
}
#endif

// Optinal step 4 during bootstrapping. Used only between an SP and a rank on a host, on the
// host side. It receives the data about the SP it is connecting to.
static dpu_offload_status_t client_ucx_boostrap_step4(execution_context_t *econtext)
{
    ucp_tag_t ucp_tag, ucp_tag_mask;
    ucp_request_param_t recv_param = {0};

    assert(econtext);
    ECONTEXT_LOCK(econtext);
    MAKE_RECV_TAG(ucp_tag,
                  ucp_tag_mask,
                  econtext->client->conn_data.oob.tag,
                  econtext->client->id,
                  econtext->client->server_id,
                  econtext->scope_id,
                  0);
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(econtext->client->bootstrapping.step4.ctx);
    recv_param.cb.recv = oob_recv_generic_handler;
    econtext->client->bootstrapping.step4.request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                                     &(econtext->engine->config->local_service_proc.info),
                                                                     sizeof(service_proc_t),
                                                                     ucp_tag,
                                                                     ucp_tag_mask,
                                                                     &recv_param);
    if (UCS_PTR_IS_ERR(econtext->client->bootstrapping.step4.request))
    {
        ERR_MSG("ucp_tag_recv_nbx() failed");
        return DO_ERROR;
    }
    if (econtext->client->bootstrapping.step4.request == NULL)
    {
        DBG("We received the local SP's data right away, callback not invoked, dealing directly with it");
        econtext->client->bootstrapping.step4.ctx.complete = true;
    }
    else
    {
        DBG("Reception of the local SP's data still in progress (engine: %p)", econtext->engine);
        assert(econtext->client->bootstrapping.step4.ctx.complete == false);
    }
    econtext->client->bootstrapping.step4.posted = true;
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
}

static dpu_offload_status_t client_ucx_boostrap_step3(execution_context_t *econtext)
{
    ECONTEXT_LOCK(econtext);
    DBG("Sent addr to server");
    // We send our "rank", i.e., unique application ID
    DBG("Sending my group/rank info (0x%x/%" PRId64 ", group seq num: %ld), len=%ld",
        econtext->rank.group_uid, econtext->rank.group_rank, econtext->rank.group_seq_num, sizeof(econtext->rank));
    ucp_request_param_t send_param = {0};
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_bootstrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.rank_ctx);
    DBG("Tag: %d; scope_id: %d", econtext->client->conn_data.oob.tag, econtext->scope_id);
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag,
                                      econtext->client->id,
                                      econtext->client->server_id,
                                      econtext->scope_id, 0);
    econtext->client->bootstrapping.rank_request = ucp_tag_send_nbx(econtext->client->server_ep,
                                                                    &(econtext->rank),
                                                                    sizeof(rank_info_t),
                                                                    ucp_tag,
                                                                    &send_param);
    if (UCS_PTR_IS_ERR(econtext->client->bootstrapping.rank_request))
    {
        ERR_MSG("ucp_tag_send_nbx() failed");
        return DO_ERROR;
    }
    if (econtext->client->bootstrapping.rank_request == NULL)
    {
        DBG("Send for rank info completed right away");
        econtext->client->bootstrapping.rank_ctx.complete = true;
        econtext->client->bootstrapping.phase = UCX_CONNECT_DONE;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
}

static dpu_offload_state_t client_ucx_bootstrap_step2(execution_context_t *econtext)
{
    ECONTEXT_LOCK(econtext);
    DBG("Sent addr size to server: %ld\n", econtext->client->conn_data.oob.local_addr_len);
    /* 2. Send the address itself */
    ucp_request_param_t send_param = {0};
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_bootstrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.addr_ctx);
    DBG("Tag: %d; scope_id: %d\n", econtext->client->conn_data.oob.tag, econtext->scope_id);
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag,
                                      econtext->client->id,
                                      econtext->client->server_id,
                                      econtext->scope_id,
                                      0);
    econtext->client->bootstrapping.addr_request = ucp_tag_send_nbx(econtext->client->server_ep,
                                                                    econtext->client->conn_data.oob.local_addr,
                                                                    econtext->client->conn_data.oob.local_addr_len,
                                                                    ucp_tag,
                                                                    &send_param);
    if (UCS_PTR_IS_ERR(econtext->client->bootstrapping.addr_request))
    {
        ERR_MSG("ucp_tag_send_nbx() failed");
        return DO_ERROR;
    }
    if (econtext->client->bootstrapping.addr_request == NULL)
    {
        DBG("Send for address completed right away");
        econtext->client->bootstrapping.addr_ctx.complete = true;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
}

static dpu_offload_status_t client_ucx_bootstrap_step1(execution_context_t *econtext)
{
    /* Establish the UCX level connection */
    ECONTEXT_LOCK(econtext);
    assert(econtext->type > 0); // should not be unknown
    assert(econtext->type < CONTEXT_LIMIT_MAX);
    ucp_ep_params_t ep_params = {0};
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address = econtext->client->conn_data.oob.peer_addr;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.arg = econtext;
    ep_params.user_data = &(econtext->client->server_ep_status);

    ucs_status_t status = ucp_ep_create(GET_WORKER(econtext), &ep_params, &(econtext->client->server_ep));
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "ucp_ep_create() failed");
    DBG("Endpoint %p successfully created", econtext->client->server_ep);

    ucp_request_param_t send_param = {0};
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_bootstrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.addr_size_ctx);

    /* 1. Send the address size */
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag,
                                      econtext->client->id,
                                      econtext->client->server_id,
                                      econtext->scope_id,
                                      0);
    DBG("Tag: %d; scope_id: %d\n", econtext->client->conn_data.oob.tag, econtext->scope_id);
    econtext->client->bootstrapping.addr_size_request = ucp_tag_send_nbx(econtext->client->server_ep, &(econtext->client->conn_data.oob.local_addr_len), sizeof(size_t), ucp_tag, &send_param);
    if (UCS_PTR_IS_ERR(econtext->client->bootstrapping.addr_size_request))
    {
        ERR_MSG("ucp_tag_send_nbx() failed");
        goto error_out;
    }
    if (econtext->client->bootstrapping.addr_size_request == NULL)
    {
        DBG("Send for my address length completed right away");
        econtext->client->bootstrapping.addr_size_ctx.complete = 1;
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

static dpu_offload_status_t oob_connect(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined client handle");
    ECONTEXT_LOCK(econtext);
    dpu_offload_client_t *client = econtext->client;
    CHECK_ERR_GOTO((client->conn_data.oob.local_addr == NULL), error_out, "undefined local address");
    DBG("local address length: %lu", client->conn_data.oob.local_addr_len);

    size_t addr_len;
    int rc = oob_client_connect(client, ai_family);
    CHECK_ERR_GOTO((rc), error_out, "oob_client_connect() failed");
    ssize_t size_recvd = recv(client->conn_data.oob.sock, &addr_len, sizeof(addr_len), MSG_WAITALL);
    DBG("Addr len received (len: %ld): %ld", size_recvd, addr_len);
    assert(size_recvd == sizeof(addr_len));
    client->conn_data.oob.peer_addr_len = addr_len;
    client->conn_data.oob.peer_addr = DPU_OFFLOAD_MALLOC(client->conn_data.oob.peer_addr_len);
    CHECK_ERR_GOTO((client->conn_data.oob.peer_addr == NULL), error_out, "Unable to allocate memory");
    size_recvd = recv(client->conn_data.oob.sock, client->conn_data.oob.peer_addr, client->conn_data.oob.peer_addr_len, MSG_WAITALL);
    DBG("Received the address (size: %ld)", size_recvd);
    assert(size_recvd == client->conn_data.oob.peer_addr_len);
    size_recvd = recv(client->conn_data.oob.sock, &(client->server_id), sizeof(client->server_id), MSG_WAITALL);
    DBG("Received the server ID (size: %ld, id: %" PRIu64 ")", size_recvd, client->server_id);
    assert(size_recvd == sizeof(client->server_id));
    size_recvd = recv(client->conn_data.oob.sock, &(client->server_global_id), sizeof(client->server_global_id), MSG_WAITALL);
    DBG("Received the server global ID (size: %ld, id: %" PRIu64 ")", size_recvd, client->server_global_id);
    assert(size_recvd == sizeof(client->server_global_id));
    size_recvd = recv(client->conn_data.oob.sock, &(client->id), sizeof(client->id), MSG_WAITALL);
    DBG("Received the client ID: %" PRIu64 " (size: %ld)", client->id, size_recvd);
    assert(size_recvd == sizeof(client->id));
    econtext->client->bootstrapping.phase = OOB_CONNECT_DONE;
    ECONTEXT_UNLOCK(econtext);
    DBG("%s() done for econtext %p (engine: %p)", __func__, econtext, econtext->engine);
    return DO_SUCCESS;

error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

/**
 * @brief Function invoked in the context of an SP when another SP gets fully connected
 * 
 * @param offload_engine Associated offload engine
 * @param remote_sp Data about the remote SP
 * @param client Client execution context used during the connection
 * @return dpu_offload_status_t 
 */
static dpu_offload_status_t
finalize_connection_to_remote_service_proc(offloading_engine_t *offload_engine, uint64_t remote_sp_idx, execution_context_t *client)
{
    ENGINE_LOCK(offload_engine);
    remote_service_proc_info_t *sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(offload_engine),
                                                       remote_sp_idx,
                                                       remote_service_proc_info_t);
    assert(sp);
    sp->ep = client->client->server_ep;
    sp->econtext = client;
    sp->addr = client->client->conn_data.oob.peer_addr;
    sp->addr_len = client->client->conn_data.oob.peer_addr_len;
    sp->ucp_worker = GET_WORKER(client);
    assert(sp->init_params.conn_params);
    DBG("Connection successfully established to service process #%" PRIu64
        " running on DPU #%" PRIu64 " (num service processes: %ld, number of connection with other service processes: %ld)",
        sp->idx,
        sp->service_proc.dpu,
        offload_engine->num_service_procs,
        offload_engine->num_connected_service_procs);
    DBG("-> Service process #%ld: addr=%s, port=%d, ep=%p, econtext=%p, client_id=%" PRIu64 ", server_id=%" PRIu64,
        sp->idx,
        sp->init_params.conn_params->addr_str,
        sp->init_params.conn_params->port,
        sp->ep,
        sp->econtext,
        client->client->id,
        client->client->server_id);
    // Do not increment num_connected_dpus, it is already done in the callback invoked when the connection goes through
    DBG("we now have %ld connections with other service processes", offload_engine->num_connected_service_procs);
    ENGINE_UNLOCK(offload_engine);
    return DO_SUCCESS;
}

static void progress_servers(offloading_engine_t *engine)
{
    size_t s;
    for (s = 0; s < engine->num_servers; s++)
    {
        execution_context_t *s_econtext = engine->servers[s];
        if (s_econtext != NULL)
        {
            s_econtext->progress(s_econtext);
        }
    }
}

dpu_offload_status_t offload_engine_progress(offloading_engine_t *engine)
{
    assert(engine);
    assert(engine->ucp_worker);

    // Progress the default UCX worker to eventually complete some communications
    ENGINE_LOCK(engine);
    ucp_worker_progress(engine->ucp_worker);
    bool on_dpu = engine->on_dpu;
    ENGINE_UNLOCK(engine);

    // Progress self_econtext
    assert(engine->self_econtext);
    engine->self_econtext->progress(engine->self_econtext);

    if (on_dpu)
    {
        // Progress connections between service processes when necessary
        size_t c, num_inter_service_proc_clients;
        ENGINE_LOCK(engine);
        num_inter_service_proc_clients = engine->num_inter_service_proc_clients;
        ENGINE_UNLOCK(engine);
        for (c = 0; c < num_inter_service_proc_clients; c++)
        {
            ENGINE_LOCK(engine);
            execution_context_t *c_econtext = engine->inter_service_proc_clients[c].client_econtext;
            ENGINE_UNLOCK(engine);
            ECONTEXT_LOCK(c_econtext);
            int initial_state = c_econtext->client->bootstrapping.phase;
            if (c_econtext != NULL)
            {
                ECONTEXT_UNLOCK(c_econtext);
                c_econtext->progress(c_econtext);
                ECONTEXT_LOCK(c_econtext);
            }
            int new_state = c_econtext->client->bootstrapping.phase;
            if (initial_state != BOOTSTRAP_DONE && new_state == BOOTSTRAP_DONE)
            {
                DBG("service proc client #%ld just finished its connection to server #%" PRIu64 ", updating data",
                    c, c_econtext->client->server_id);
                ECONTEXT_UNLOCK(c_econtext);
                dpu_offload_status_t rc = finalize_connection_to_remote_service_proc(engine,
                                                                                     engine->inter_service_proc_clients[c].remote_service_proc_idx,
                                                                                     engine->inter_service_proc_clients[c].client_econtext);
                CHECK_ERR_RETURN((rc), DO_ERROR, "finalize_connection_to_remote_service_proc() failed");
                ECONTEXT_UNLOCK(c_econtext);
            }
            ECONTEXT_UNLOCK(c_econtext);
        }
        progress_servers(engine);

        // Progress all the execution context used to connect to other service processes
        assert(engine->num_service_procs);
        size_t i;
        for (i = 0; i < engine->num_service_procs; i++)
        {
            remote_service_proc_info_t *sp;
            sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(engine), i, remote_service_proc_info_t);
            assert(sp);
            if (sp == NULL)
                continue;

            execution_context_t *econtext = sp->econtext;
            if (econtext == NULL || econtext->progress == NULL)
                continue;

            econtext->progress(econtext);
        }
    }
    else
    {
        if (engine->client != NULL)
        {
            engine->client->progress(engine->client);
        }
        progress_servers(engine);
    }
    return DO_SUCCESS;
}

dpu_offload_status_t lib_progress(execution_context_t *econtext)
{
    if (econtext->engine != NULL)
        return offload_engine_progress(econtext->engine);
    else
        econtext->progress(econtext);
    return DO_SUCCESS;
}

void init_remote_dpu_info_struct(void *data)
{
    assert(data);
    remote_dpu_info_t *d = (remote_dpu_info_t *)data;
    RESET_REMOTE_DPU_INFO(d);
}

static void init_pending_am_rdv_recv_t(void *ptr)
{
    pending_am_rdv_recv_t *elt = (pending_am_rdv_recv_t *)ptr;
    RESET_PENDING_RDV_RECV(elt);
}

extern dpu_offload_status_t engine_get_env_config(offloading_engine_t *engine);
dpu_offload_status_t offload_engine_init(offloading_engine_t **engine)
{
    int ret;
    dpu_offload_status_t rc;
    offloading_engine_t *d = DPU_OFFLOAD_MALLOC(sizeof(offloading_engine_t));
    CHECK_ERR_GOTO((d == NULL), error_out, "Unable to allocate resources");
    RESET_ENGINE(d, ret);
    // Check that everthing is fine
    CHECK_ERR_GOTO((ret != 0), error_out, "Engine initialization failed");
    CHECK_ERR_GOTO((d->servers == NULL), error_out, "unable to allocate memory to track servers");
    CHECK_ERR_GOTO((d->servers == NULL), error_out, "unable to allocate resources");
    CHECK_ERR_GOTO((d->free_op_descs == NULL), error_out, "Allocation of pool of free operation descriptors failed");
    CHECK_ERR_GOTO((d->free_cache_entry_requests == NULL), error_out, "Allocations of pool of descriptions for cache queries failed");
    CHECK_ERR_GOTO((d->pool_conn_params == NULL), error_out, "Allocation of pool of connection parameter descriptors failed");

#if BUDDY_BUFFER_SYS_ENABLE
    d->settings.buddy_buffer_system_enabled = true;
#else
    d->settings.buddy_buffer_system_enabled = false;
#endif // BUDDY_BUFFER_SYS_ENABLE

#if USE_AM_IMPLEM
    d->settings.ucx_am_backend_enabled = true;
#else
    d->settings.ucx_am_backend_enabled = false;
#endif // USE_AM_IMPLEM

    rc = engine_get_env_config(d);
    CHECK_ERR_GOTO((rc != DO_SUCCESS), error_out, "engine_get_env_config() failed");

    if (d->settings.buddy_buffer_system_enabled)
        SMART_BUFFS_INIT(&(d->smart_buffer_sys), NULL);
    if (d->settings.ucx_am_backend_enabled)
    {
        DYN_LIST_ALLOC_WITH_INIT_CALLBACK(d->free_pending_rdv_recv,
                                          32,
                                          pending_am_rdv_recv_t,
                                          item,
                                          init_pending_am_rdv_recv_t);
        ucs_list_head_init(&(d->pending_rdv_recvs));
        d->client_lookup_table = kh_init(client_lookup_hash_t);
    }

    /* Some initialization of the group/endpoint cache */
    DYN_LIST_ALLOC(d->free_sp_cache_hash_obj,
                   1024,
                   sp_cache_data_t,
                   item);
    DYN_LIST_ALLOC(d->free_host_cache_hash_obj,
                   32,
                   host_cache_data_t,
                   item);
    d->procs_cache.engine = d;

    rc = ev_channels_init(&(d->default_notifications));
    CHECK_ERR_GOTO((rc), error_out, "ev_channels_init() failed");

    /* INITIALIZE THE SELF EXECUTION CONTEXT */
    execution_context_t *self_econtext = NULL;
    DBG("initializing execution context...");
    rc = execution_context_init(d, CONTEXT_SELF, &self_econtext);
    CHECK_ERR_GOTO((rc), error_out, "execution_context_init() failed");
    self_econtext->scope_id = SCOPE_SELF;
    DBG("execution context created (econtext: %p, scope_id: %d)", self_econtext, self_econtext->scope_id);
    d->self_econtext = self_econtext;
    rc = event_channels_init(self_econtext);
    CHECK_ERR_GOTO((rc), error_out, "event_channels_init() failed");
    CHECK_ERR_GOTO((self_econtext->event_channels == NULL), error_out, "event channel handle is undefined");
    self_econtext->event_channels->econtext = self_econtext;
    DBG("event channel %p successfully initialized (econtext: %p, scope_id: %d)",
        self_econtext->event_channels,
        self_econtext,
        self_econtext->scope_id);

    *engine = d;
    return DO_SUCCESS;
error_out:
    if (d->servers)
        free(d->servers);
    if (d->inter_service_proc_clients)
        free(d->inter_service_proc_clients);
    if (d->free_op_descs)
        DYN_LIST_FREE(d->free_op_descs, op_desc_t, item);
    if (d->free_cache_entry_requests)
        DYN_LIST_FREE(d->free_cache_entry_requests, cache_entry_request_t, item);
    if (d->free_sp_cache_hash_obj)
        DYN_LIST_FREE(d->free_sp_cache_hash_obj, sp_cache_data_t, item);
    if (d->free_host_cache_hash_obj)
        DYN_LIST_FREE(d->free_host_cache_hash_obj, host_cache_data_t, item);
    if (d->self_econtext)
        execution_context_fini(&d->self_econtext);
    *engine = NULL;
    return DO_ERROR;
}

dpu_offload_status_t offload_engine_init_with_info(offloading_engine_t **engine, offloading_engine_info_t *info)
{
    dpu_offload_status_t rc;

    rc = offload_engine_init(engine);
    CHECK_ERR_RETURN((rc), DO_ERROR, "offload_engine_init() failed");
    assert(engine);
    assert(*engine);
    if (info)
    {
        (*engine)->on_dpu = info->on_dpu;
    }

    if ((*engine)->on_dpu)
    {
        RESET_DPU_ENGINE(*engine);
    }

    return DO_SUCCESS;
}

#if !USE_AM_IMPLEM
static void progress_server_event_recv(execution_context_t *econtext)
{
    size_t n_client = 0;
    size_t idx = 0;
    int rc;
    ECONTEXT_LOCK(econtext);
    while (n_client < econtext->server->connected_clients.num_connected_clients)
    {
        peer_info_t *client_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), idx, peer_info_t);
        if (client_info == NULL || client_info->bootstrapping.phase != BOOTSTRAP_DONE)
        {
            idx++;
            continue;
        }

        ucp_worker_h worker = GET_WORKER(econtext);
        if (client_info->notif_recv.initialized == false)
        {
            // Note: tag and tag mask are calculated by the macro to match the client
            // i.e., only messages from that client will be received.
            DBG("Preparing reception of notification from client #%ld (econtext: %p, scope_id: %d, peer_id: %ld, rank ID: %ld)",
                idx,
                econtext,
                econtext->scope_id,
                client_info->id,
                client_info->rank_data.group_rank);

#if !NDEBUG
            if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_INTER_SERVICE_PROCS)
            {
                if (client_info->id >= econtext->engine->num_service_procs)
                    ERR_MSG("requested client ID is invalid: %" PRIu64, client_info->id);
                assert(client_info->id < econtext->engine->num_service_procs);
            }
#endif

            // Always use the unique global service process ID from the list otherwise we can have a server and a client with the same ID
            // and receives would not work as expected since they are posted on the same worker with ultimately the same
            // tag
            PREP_NOTIF_RECV(client_info->notif_recv.ctx,
                            client_info->notif_recv.hdr_recv_params,
                            client_info->notif_recv.hdr_ucp_tag,
                            client_info->notif_recv.hdr_ucp_tag_mask,
                            worker,
                            client_info->id,
                            econtext->server->id,
                            econtext->scope_id);
            client_info->notif_recv.initialized = true;
        }
        // The function will handle whether a new receive is required to be posted or not
        ECONTEXT_UNLOCK(econtext);
        do
        {
            rc = post_new_notif_recv(worker,
                                     &(client_info->notif_recv.ctx),
                                     econtext,
                                     client_info->notif_recv.hdr_ucp_tag,
                                     client_info->notif_recv.hdr_ucp_tag_mask,
                                     &(client_info->notif_recv.hdr_recv_params));
            if (rc == -1)
                ERR_MSG("post_new_notif_recv() failed");
        } while (rc == EVENT_DONE);
        ECONTEXT_LOCK(econtext);
        idx++;
        n_client++;
    }
    ECONTEXT_UNLOCK(econtext);
}

static void progress_client_event_recv(execution_context_t *econtext)
{
    int rc;
    ECONTEXT_LOCK(econtext);
    ucp_worker_h worker = GET_WORKER(econtext);
    if (econtext->event_channels->notif_recv.initialized == false)
    {
        // Note: tag and tag mask are calculated by the macro to match the client
        // i.e., only messages from the sever will be received.
        DBG("Preparing reception of notification from server (econtext: %p, scope_id: %d, peer_id: %ld)",
            econtext,
            econtext->scope_id,
            econtext->client->server_id);

#if !NDEBUG
        if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_INTER_SERVICE_PROCS)
        {
            if (econtext->client->id >= econtext->engine->num_service_procs)
                ERR_MSG("requested client ID is invalid: %" PRIu64, econtext->client->id);
            assert(econtext->client->id < econtext->engine->num_service_procs);
        }
#endif

        PREP_NOTIF_RECV(econtext->event_channels->notif_recv.ctx,
                        econtext->event_channels->notif_recv.hdr_recv_params,
                        econtext->event_channels->notif_recv.hdr_ucp_tag,
                        econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                        worker,
                        econtext->client->id,
                        econtext->client->server_id,
                        econtext->scope_id);
        econtext->event_channels->notif_recv.initialized = true;
    }
    ECONTEXT_UNLOCK(econtext);

    // The function will handle whether a new receive is required to be posted or not
    do
    {
        rc = post_new_notif_recv(worker,
                                 &(econtext->event_channels->notif_recv.ctx),
                                 econtext,
                                 econtext->event_channels->notif_recv.hdr_ucp_tag,
                                 econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                                 &(econtext->event_channels->notif_recv.hdr_recv_params));
        if (rc == -1)
            ERR_MSG("post_new_notif_recv() failed");
    } while (rc == EVENT_DONE);
}

static void progress_self_event_recv(execution_context_t *econtext)
{
    int rc;
    ECONTEXT_LOCK(econtext);
    ucp_worker_h worker = GET_WORKER(econtext);
    if (econtext->event_channels == NULL)
    {
        // Event channels are not initialized yet, exit
        return;
    }

    if (econtext->event_channels->notif_recv.initialized == false)
    {
        // Note: tag and tag mask are calculated by the macro to match the client
        // i.e., only messages from the sever will be received.
        PREP_NOTIF_RECV(econtext->event_channels->notif_recv.ctx,
                        econtext->event_channels->notif_recv.hdr_recv_params,
                        econtext->event_channels->notif_recv.hdr_ucp_tag,
                        econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                        worker,
                        0UL, // context ID is always 0 for self notifications (a.k.a. local notifications)
                        0UL, // context ID is always 0 for self notifications (a.k.a. local notifications)
                        econtext->scope_id);
        econtext->event_channels->notif_recv.initialized = true;
    }
    ECONTEXT_UNLOCK(econtext);

    // The function will handle whether a new receive is required to be posted or not
    do
    {
        rc = post_new_notif_recv(worker,
                                 &(econtext->event_channels->notif_recv.ctx),
                                 econtext,
                                 econtext->event_channels->notif_recv.hdr_ucp_tag,
                                 econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                                 &(econtext->event_channels->notif_recv.hdr_recv_params));
        if (rc == -1)
            ERR_MSG("post_new_notif_recv() failed");
    } while (rc == EVENT_DONE);
}
#endif // !USE_AM_IMPLEM

static void progress_event_recv(execution_context_t *econtext)
{
#if USE_AM_IMPLEM
    /* Nothing to do */
#else
    // Progress the ongoing communications
    switch (econtext->type)
    {
    case CONTEXT_SERVER:
        progress_server_event_recv(econtext);
        break;
    case CONTEXT_CLIENT:
        progress_client_event_recv(econtext);
        break;
    case CONTEXT_SELF:
        progress_self_event_recv(econtext);
        break;
    default:
        ERR_MSG("invalid execution context type (%d)", econtext->type);
    }
#endif
}

void local_rank_connect_default_callback(void *data)
{
    connected_peer_data_t *connected_peer;
    uint64_t peer_id;
    peer_info_t *client_info;
    group_uid_t group_uid;
    group_cache_t *gc;

    assert(data);
    connected_peer = (connected_peer_data_t *)data;
    assert(connected_peer->econtext);

    // Look up the details about the peer
    peer_id = connected_peer->peer_id;
    DBG("Finalizing connection with rank on the host (client #%ld)", peer_id);
    client_info = DYN_ARRAY_GET_ELT(&(connected_peer->econtext->server->connected_clients.clients), peer_id, peer_info_t);
    assert(client_info);
    group_uid = client_info->rank_data.group_uid;

    if (group_uid == INT_MAX)
    {
        DBG("No group associated to peer that is finalizing connection, not dealing with cache aspects");
        return;
    }

    assert(connected_peer->econtext->engine);
    gc = GET_GROUP_CACHE_INTERNAL(&(connected_peer->econtext->engine->procs_cache),
                         group_uid,
                         client_info->rank_data.group_size);
    assert(gc);
    DBG("Checking group 0x%x (number of local entries: %ld, n_local_populated: %ld, n_local: %ld)",
        group_uid, gc->n_local_ranks_populated, gc->n_local_ranks_populated, gc->n_local_ranks);
    assert(gc->n_local_ranks_populated <= gc->group_size);

    // If the cache is full, i.e., all the ranks of the group are on the host,
    // or if the entire cache is there, we make sure to send it to the local ranks if necessary
    if (gc->group_size == gc->num_local_entries)
    {
        DBG("Cache for group 0x%x is now complete, sending it to the local ranks (scope_id: %d, num connected clients: %ld)",
            group_uid, connected_peer->econtext->scope_id, connected_peer->econtext->server->connected_clients.num_connected_clients);
        dpu_offload_status_t rc = send_gp_cache_to_host(connected_peer->econtext, group_uid);
        CHECK_ERR_GOTO((rc), error_out, "send_gp_cache_to_host() failed");
    }

    // No need to trigger the broadcast of the cache, it is handled by the inter-dpu code.
    return;
error_out:
    ERR_MSG("unable to figure out if cache needs to be sent out to local ranks");
    return;
}

/**
 * @brief add_cache_entry_for_new_client is called by a service process once a process
 * on the host finalizes its connection; it adds the newly connected process to the
 * group cache when applicable.
 *
 * @param client_info Data about the newly connected client
 * @param ctx Execution context in the context of which the data was received
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t
add_cache_entry_for_new_client(peer_info_t *client_info, execution_context_t *ctx)
{

    if (client_info->rank_data.group_uid != INT_MAX &&
        client_info->rank_data.group_rank != INVALID_RANK)
    {
        // Update the pointer to track cache entries, i.e., groups/ranks, for the peer
        peer_cache_entry_t *cache_entry = NULL;
        dpu_offload_status_t rc;
        group_cache_t *gp_cache = NULL;

        gp_cache = GET_GROUP_CACHE_INTERNAL(&(ctx->engine->procs_cache),
                                   client_info->rank_data.group_uid,
                                   client_info->rank_data.group_size);
        assert(gp_cache);

        DBG("Adding gp/rank 0x%x/%" PRId64 " to cache for group 0x%x (group's seq_num: %ld)",
            client_info->rank_data.group_uid,
            client_info->rank_data.group_rank,
            gp_cache->group_uid,
            gp_cache->persistent.num);

        // Since the client just connected, it is by definition the first group, i.e.,
        // MPI_COMM_WORLD in the context of MPI. So the group is either not know yet (num == 0) 
        // or known because one rank or more joined (num == 1)
        assert(gp_cache->persistent.num == 0 || gp_cache->persistent.num == 1);
        // Since this happens in the context of the client bootstrapping and therefore
        // for the first group, we explicitely set the group's sequence number to 1.
        if (gp_cache->persistent.num == 0)
            gp_cache->persistent.num = 1;

        cache_entry = GET_GROUP_RANK_CACHE_ENTRY(&(ctx->engine->procs_cache),
                                                 client_info->rank_data.group_uid,
                                                 client_info->rank_data.group_rank,
                                                 client_info->rank_data.group_size);
        assert(cache_entry);
        COPY_RANK_INFO(&(client_info->rank_data), &(cache_entry->peer.proc_info));
        cache_entry->client_id = client_info->id;
        cache_entry->num_shadow_service_procs = 1;
        cache_entry->peer.host_info = client_info->rank_data.host_info;
        if (ctx->engine->on_dpu)
        {
            cache_entry->shadow_service_procs[0] = ctx->engine->config->local_service_proc.info.global_id;
        }
        else
        {
            cache_entry->shadow_service_procs[0] = ECONTEXT_ID(ctx);
        }
        assert(cache_entry->ep == NULL);
        cache_entry->set = true;

        if (gp_cache->n_local_ranks <= 0 && client_info->rank_data.n_local_ranks >= 0)
        {
            DBG("Setting the number of local ranks for the group 0x%x (%" PRId64 ")",
                client_info->rank_data.group_uid,
                client_info->rank_data.n_local_ranks);
            gp_cache->n_local_ranks = client_info->rank_data.n_local_ranks;
        }
        gp_cache->n_local_ranks_populated++;
        gp_cache->num_local_entries++;
        cache_entry->client_id = client_info->id;
        cache_entry->peer.addr_len = client_info->peer_addr_len;
        if (client_info->peer_addr != NULL)
        {
            assert(client_info->peer_addr_len < MAX_ADDR_LEN);
            memcpy(cache_entry->peer.addr,
                   client_info->peer_addr,
                   client_info->peer_addr_len);
        }
        assert(client_info->cache_entry_index_data.capacity > 0);
        peer_cache_entry_index_t *index_item = DYN_ARRAY_GET_ELT(&(client_info->cache_entry_index_data),
                                                                 0,
                                                                 peer_cache_entry_index_t);
        assert(index_item);
        index_item->group_uid = client_info->rank_data.group_uid;
        index_item->rank = client_info->rank_data.group_rank;
        index_item->group_size = client_info->rank_data.group_size;

        // Update the topology
        rc = update_topology_data(ctx->engine,
                                  gp_cache,
                                  client_info->rank_data.group_rank,
                                  ctx->engine->config->local_service_proc.info.global_id,
                                  ctx->engine->config->local_service_proc.host_uid);
        CHECK_ERR_RETURN((rc), DO_ERROR, "update_topology_data() failed");
    }
    return DO_SUCCESS;
}

static dpu_offload_status_t finalize_client_connection(execution_context_t *econtext, peer_info_t *client_info, size_t idx)
{
    dpu_offload_status_t rc;
    client_info->bootstrapping.phase = BOOTSTRAP_DONE;
    econtext->server->connected_clients.num_ongoing_connections--;
    econtext->server->connected_clients.num_connected_clients++;
    econtext->server->connected_clients.num_total_connected_clients++;
    DBG("****** Bootstrapping of client #%ld now completed in scope %d (econtext: %p, engine: %p), %ld are now connected (connected service processes: %ld)",
        idx,
        econtext->scope_id,
        econtext,
        econtext->engine,
        econtext->server->connected_clients.num_connected_clients,
        econtext->engine->num_connected_service_procs);

    // Trigger the exchange of the cache between service processes when all the local ranks are connected
    // Do not check for errors, it may fail at this point (if all service processes are not connected) and it is okay
    if (ECONTEXT_ON_DPU(econtext) &&
        econtext->scope_id == SCOPE_HOST_DPU &&
        client_info->rank_data.group_uid != INT_MAX &&
        client_info->rank_data.group_rank != INVALID_RANK)
    {
        // add_cache_entry_for_new_client checks if the data actually needs to be put in the cache
        rc = add_cache_entry_for_new_client(client_info, econtext);
        if (rc == DO_ERROR)
        {
            ECONTEXT_UNLOCK(ctx);
            return DO_ERROR;
        }

        if (econtext->engine->host_id == UINT64_MAX)
            econtext->engine->host_id = client_info->rank_data.host_info;
        GROUP_CACHE_EXCHANGE(econtext->engine,
                             client_info->rank_data.group_uid,
                             client_info->rank_data.n_local_ranks);
    }

    if (econtext->server->connected_cb != NULL)
    {
        DBG("Invoking connection completion callback...");
        connected_peer_data_t cb_data = {
            .addr = client_info->peer_addr,
            .addr_len = client_info->peer_addr_len,
            .econtext = econtext,
            .peer_id = idx,
            .rank_info = client_info->rank_data,
        };
        econtext->server->connected_cb(&cb_data);
    }
    return DO_SUCCESS;
}

static void progress_server_econtext(execution_context_t *ctx)
{
    dpu_offload_status_t rc;
    if (ctx->server->connected_clients.num_ongoing_connections > 0)
    {
        size_t i = 0; // Number of ongoing connections we already handled
        size_t idx = 0;
        // Find the clients that are currently connecting, it can be any client from the list but we know
        // how many are currently in the process of connecting.
        while (i < ctx->server->connected_clients.num_ongoing_connections && idx < ctx->server->connected_clients.clients.capacity)
        {
            peer_info_t *client_info = DYN_ARRAY_GET_ELT(&(ctx->server->connected_clients.clients), idx, peer_info_t);
            assert(client_info);
            if ((client_info->bootstrapping.phase == OOB_CONNECT_DONE || client_info->bootstrapping.phase == UCX_CONNECT_DONE) && client_info->bootstrapping.phase != BOOTSTRAP_DONE)
            {
                // OOB connection established, we can initiate the UCX bootstrapping.
                if (client_info->bootstrapping.addr_size_ctx.complete == false && client_info->bootstrapping.addr_size_request == NULL)
                {
                    DBG("UCX level bootstrap - client #%" PRIu64 ", step 1 - Getting address size", idx);
                    rc = oob_server_ucx_client_connection_step1(ctx, client_info);
                    if (rc)
                    {
                        ERR_MSG("oob_server_ucx_client_connection_step1() failed");
                        return;
                    }
                }

                if (client_info->bootstrapping.addr_size_ctx.complete == true && client_info->bootstrapping.addr_size_request != NULL)
                {
                    // The send did not complete right away but now is, some clean up to do
                    ucp_request_free(client_info->bootstrapping.addr_size_request);
                    client_info->bootstrapping.addr_size_request = NULL;
                }

                if (client_info->bootstrapping.addr_size_ctx.complete == true && client_info->bootstrapping.addr_ctx.complete == false && client_info->bootstrapping.addr_request == NULL)
                {
                    DBG("UCX level bootstrap - client #%" PRIu64 ", step 2 - Getting address", idx);
                    rc = oob_server_ucx_client_connection_step2(ctx, client_info);
                    if (rc)
                    {
                        ERR_MSG("oob_server_ucx_client_connection_step2() failed");
                        return;
                    }
                }

                if (client_info->bootstrapping.addr_ctx.complete == true && client_info->bootstrapping.addr_request != NULL)
                {
                    // The send did not complete right away but now is, some clean up to do
                    ucp_request_free(client_info->bootstrapping.addr_request);
                    client_info->bootstrapping.addr_request = NULL;
                }

                if (client_info->bootstrapping.addr_ctx.complete == true && client_info->bootstrapping.rank_ctx.complete == false && client_info->bootstrapping.rank_request == NULL)
                {
                    DBG("UCX level bootstrap - client #%" PRIu64 ", step 3 - Getting rank info", idx);
                    rc = oob_server_ucx_client_connection_step3(ctx, client_info);
                    if (rc)
                    {
                        ERR_MSG("oob_server_ucx_client_connection_step3() failed");
                        return;
                    }
                }

                if (client_info->bootstrapping.rank_ctx.complete == true)
                {
                    if (client_info->bootstrapping.rank_request != NULL)
                    {
                        // The send did not complete right away but now is, some clean up to do
                        ucp_request_free(client_info->bootstrapping.rank_request);
                        client_info->bootstrapping.rank_request = NULL;
                    }

                    // If we got the rank info but still in the OOB_CONNECT_DONE phase,
                    // it means we received all the data but did not update the local cache
                    // and fully complete the local initialization for the client.
                    ECONTEXT_LOCK(ctx);
                    client_info->bootstrapping.phase = UCX_CONNECT_DONE;
                    DBG("group/rank received: (0x%x/%" PRId64 "); group_size: %ld, local ranks: %ld",
                        client_info->rank_data.group_uid,
                        client_info->rank_data.group_rank,
                        client_info->rank_data.group_size,
                        client_info->rank_data.n_local_ranks);

                    // By default, we assume we do not have to go through step 4.
                    client_info->bootstrapping.step4.ctx.complete = true;

                    if (ECONTEXT_ON_DPU(ctx))
                    {
                        if (ctx->scope_id == SCOPE_INTER_SERVICE_PROCS)
                        {
                            // Inter-SP connection
                            size_t service_proc = client_info->rank_data.group_rank;
                            remote_service_proc_info_t *sp = DYN_ARRAY_GET_ELT(GET_ENGINE_LIST_SERVICE_PROCS(ctx->engine),
                                                                            service_proc,
                                                                            remote_service_proc_info_t);
                            assert(sp);
                            assert(service_proc < ctx->engine->num_service_procs);
                            assert(ctx->engine);
                            // Set the endpoint to communicate with that remote service process
                            sp->ep = client_info->ep;
                            // Set the pointer to the execution context in the list of know service processes. Used for notifications with the remote service process.
                            sp->econtext = ctx;
                            DBG("-> Service process #%ld: addr=%s, port=%d, ep=%p, econtext=%p, client_id=%" PRIu64 ", server_id=%" PRIu64 "", service_proc,
                                sp->init_params.conn_params->addr_str,
                                sp->init_params.conn_params->port,
                                sp->ep,
                                ctx,
                                client_info->id,
                                ctx->server->id);
                        }
                        else
                        {
                            if (!client_info->bootstrapping.step4.posted)
                            {
                                // SP-host connection, from the SP
                                // Send our global since the host process has no way to calculate it (not enough context)
                                client_info->bootstrapping.step4.ctx.complete = false;
                                rc = oob_server_ucx_client_connection_step4(ctx, client_info);
                                if (rc)
                                {
                                    ERR_MSG("oob_server_ucx_client_connection_step3() failed");
                                    return;
                                }
                                client_info->bootstrapping.step4.posted = true;
                            }
                        }
                    }

                    ECONTEXT_UNLOCK(ctx);
                }

                if (client_info->bootstrapping.step4.ctx.complete)
                {
                    if (client_info->bootstrapping.step4.request != NULL)
                    {
                        ucp_request_free(client_info->bootstrapping.step4.request);
                        client_info->bootstrapping.step4.request = NULL;
                    }

                    rc = finalize_client_connection(ctx, client_info, idx);
                    if (rc)
                    {
                        ERR_MSG("finalize_client_connection() failed");
                        return;
                    }
                }

                i++; // We just finished handling a client in the process of connecting
            }
            idx++;
        }
    }
}

static void progress_client_econtext(execution_context_t *ctx)
{
    dpu_offload_status_t rc;

    if (ctx->client->bootstrapping.phase == OOB_CONNECT_DONE)
    {
        // // We now have a OOB connection established; we need now to progress UCX bootstrapping
        if (ctx->client->bootstrapping.addr_size_ctx.complete == false && ctx->client->bootstrapping.addr_size_request == NULL)
        {
            DBG("UCX level bootstrap - step 1");
            rc = client_ucx_bootstrap_step1(ctx);
            if (rc)
            {
                ERR_MSG("client_ucx_bootstrap_step1() failed");
                return;
            }
        }

        if (ctx->client->bootstrapping.addr_size_ctx.complete == true && ctx->client->bootstrapping.addr_size_request != NULL)
        {
            // The send did not complete right away but now is, some clean up to do
            ucp_request_free(ctx->client->bootstrapping.addr_size_request);
            ctx->client->bootstrapping.addr_size_request = NULL;
        }

        if (ctx->client->bootstrapping.addr_size_ctx.complete == true && ctx->client->bootstrapping.addr_ctx.complete == false && ctx->client->bootstrapping.addr_request == NULL)
        {
            DBG("UCX level boostrap - step 2");
            rc = client_ucx_bootstrap_step2(ctx);
            if (rc)
            {
                ERR_MSG("client_ucx_boostrap_step2() failed");
                return;
            }
        }

        if (ctx->client->bootstrapping.addr_ctx.complete == true && ctx->client->bootstrapping.addr_request != NULL)
        {
            // The send did not complete right away but now is, some clean up to do
            ucp_request_free(ctx->client->bootstrapping.addr_request);
            ctx->client->bootstrapping.addr_request = NULL;
        }

        if (ctx->client->bootstrapping.addr_ctx.complete == true && ctx->client->bootstrapping.rank_ctx.complete == false && ctx->client->bootstrapping.rank_request == NULL)
        {
            DBG("UCX level boostrap - step 3");
            rc = client_ucx_boostrap_step3(ctx);
            if (rc)
            {
                ERR_MSG("client_ucx_boostrap_step3() failed");
                return;
            }
        }

        if (ctx->client->bootstrapping.rank_ctx.complete == true && ctx->client->bootstrapping.rank_request != NULL)
        {
            // The send did not complete right away but now is, some clean up to do
            ucp_request_free(ctx->client->bootstrapping.rank_request);
            ctx->client->bootstrapping.rank_request = NULL;
            ctx->client->bootstrapping.phase = UCX_CONNECT_DONE;
        }
    }

    if (ctx->client->bootstrapping.phase == UCX_CONNECT_DONE)
    {
        // We now have a UCX connection established

        // If we are on the host and in the context of DPU-host bootstrapping, we need to receive some data from
        // the SP to precisely know who it is.
        if (!ECONTEXT_ON_DPU(ctx) && ctx->scope_id == SCOPE_HOST_DPU)
        {
            if (ctx->client->bootstrapping.rank_ctx.complete == true && ctx->client->bootstrapping.step4.ctx.complete == false && !ctx->client->bootstrapping.step4.posted)
            {
                // All previous steps completed, time to post the receive to get the SP's data
                rc = client_ucx_boostrap_step4(ctx);
                if (rc)
                {
                    ERR_MSG("client_ucx_boostrap_step4() failed");
                    return;
                }
            }

            if (ctx->client->bootstrapping.step4.ctx.complete)
            {
                if (ctx->client->bootstrapping.step4.request != NULL)
                {
                    ucp_request_free(ctx->client->bootstrapping.step4.request);
                    ctx->client->bootstrapping.step4.request = NULL;
                }
            }
        }
        else
            ctx->client->bootstrapping.step4.ctx.complete = true;

        if (ctx->client->bootstrapping.addr_size_ctx.complete == true &&
            ctx->client->bootstrapping.addr_ctx.complete == true &&
            ctx->client->bootstrapping.rank_ctx.complete == true &&
            ctx->client->bootstrapping.addr_size_request == NULL &&
            ctx->client->bootstrapping.addr_request == NULL &&
            ctx->client->bootstrapping.rank_request == NULL &&
            ctx->client->bootstrapping.step4.ctx.complete == true &&
            ctx->client->bootstrapping.step4.request == NULL)
        {
            ctx->client->bootstrapping.phase = BOOTSTRAP_DONE;

            // Bootstrapping in completed, we have all the necessary data to
            // be able to safely add ourselves to the local EP cache as shadow service process
            if (ctx->rank.group_uid != INT_MAX &&
                ctx->rank.group_rank != INVALID_RANK)
            {
                group_cache_t *gp_cache = NULL;
                assert(ctx->engine->config->local_service_proc.info.global_id != UINT64_MAX);
                gp_cache = GET_GROUP_CACHE_INTERNAL(&(ctx->engine->procs_cache),
                                           ctx->rank.group_uid,
                                           ctx->rank.group_size);
                assert(gp_cache);
                rc = update_topology_data(ctx->engine,
                                          gp_cache,
                                          ctx->rank.group_rank,
                                          ctx->engine->config->local_service_proc.info.global_id,
                                          ctx->rank.host_info);
                if (rc != DO_SUCCESS)
                {
                    ERR_MSG("update_topology_data() failed");
                    return;
                }
            }

            if (ctx->client->connected_cb != NULL)
            {
                DBG("Successfully connected, invoking connected callback (econtext: %p, client: %p, cb: %p)",
                    ctx, ctx->client, ctx->client->connected_cb);
                connected_peer_data_t cb_data;
                assert(ctx->client);
                assert(ctx->client->conn_params.addr_str);
                cb_data.addr = ctx->client->conn_params.addr_str;
                cb_data.addr_len = strlen(ctx->client->conn_params.addr_str);
                cb_data.econtext = ctx;
                cb_data.peer_id = ctx->client->server_id;
                cb_data.global_peer_id = ctx->client->server_global_id;
                cb_data.rank_info = ctx->rank;
                ctx->client->connected_cb(&cb_data);
            }
        }
    }
}

/**
 * @brief term_notification_completed is the funtion invoked once we get the completion of the term notification.
 * Upon completion, we know we can safely
 *
 * @param econtext
 */
static void term_notification_completed(execution_context_t *econtext)
{
    // Only clients are supposed to send the term message
    assert(econtext->type == CONTEXT_CLIENT);

    if (econtext->client->server_ep)
    {
        // FIXME: this is creating a crash
        // ep_close(GET_WORKER(econtext), econtext->client->server_ep);
        econtext->client->server_ep = NULL;
    }

    // Free memory allocated during bootstrapping
    switch (econtext->client->mode)
    {
    case UCX_LISTENER:
        break;
    default:
        // OOB
        if (econtext->client->conn_data.oob.sock > 0)
        {
            close(econtext->client->conn_data.oob.sock);
            econtext->client->conn_data.oob.sock = -1;
        }
        if (econtext->client->conn_data.oob.addr_msg_str != NULL)
        {
            free(econtext->client->conn_data.oob.addr_msg_str);
            econtext->client->conn_data.oob.addr_msg_str = NULL;
        }
        if (econtext->client->conn_data.oob.peer_addr != NULL)
        {
            free(econtext->client->conn_data.oob.peer_addr);
            econtext->client->conn_data.oob.peer_addr = NULL;
        }

        if (econtext->client->conn_data.oob.local_addr)
        {
            ucp_worker_release_address(GET_WORKER(econtext), econtext->client->conn_data.oob.local_addr);
            econtext->client->conn_data.oob.local_addr = NULL;
        }
    }

    switch (econtext->type)
    {
    case CONTEXT_CLIENT:
    {
        // Free resources to receive notifications
#if !USE_AM_IMPLEM
        if (!econtext->engine->settings.ucx_am_backend_enabled)
        {
            /* WE ARE NOT USING THE AM BACKEND */
            if (econtext->event_channels->notif_recv.ctx.req != NULL)
            {
                ucp_request_cancel(GET_WORKER(econtext), econtext->event_channels->notif_recv.ctx.req);
                ucp_request_release(econtext->event_channels->notif_recv.ctx.req);
                econtext->event_channels->notif_recv.ctx.req = NULL;
            }
            if (econtext->event_channels->notif_recv.ctx.payload_ctx.req != NULL)
            {
                ucp_request_cancel(GET_WORKER(econtext), econtext->event_channels->notif_recv.ctx.payload_ctx.req);
                ucp_request_release(econtext->event_channels->notif_recv.ctx.payload_ctx.req);
                econtext->event_channels->notif_recv.ctx.payload_ctx.req = NULL;
            }
            if (econtext->engine->settings.buddy_buffer_system_enabled)
            {
                if (econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf != NULL)
                {
                    SMART_BUFF_RETURN(&(econtext->engine->smart_buffer_sys),
                                      econtext->event_channels->notif_recv.ctx.hdr.payload_size,
                                      econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf);
                    econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf = NULL;
                }
            }
            else
            {
                if (econtext->event_channels->notif_recv.ctx.payload_ctx.buffer != NULL)
                {
                    free(econtext->event_channels->notif_recv.ctx.payload_ctx.buffer);
                    econtext->event_channels->notif_recv.ctx.payload_ctx.buffer = NULL;
                }
            }
        }
#endif // !USE_AM_IMPLEM

        econtext->client->done = true;
        break;
    }
    case CONTEXT_SERVER:
    {
// Free resources to receive notifications
#if !USE_AM_IMPLEM
#endif
        econtext->server->done = true;
        break;
    }
    default:
    {
        ERR_MSG("invalid execution context type (%d)", econtext->type);
    }
    }
}

static void execution_context_progress(execution_context_t *econtext)
{
    assert(econtext);

    // Progress the UCX worker to eventually complete some communications
    ucp_worker_h worker = GET_WORKER(econtext);
    ucp_worker_progress(worker);

    switch (econtext->type)
    {
    case CONTEXT_SERVER:
        progress_server_econtext(econtext);
        break;
    case CONTEXT_CLIENT:
        progress_client_econtext(econtext);
        break;
    default:
        break;
    }

    // Progress reception of events
    progress_event_recv(econtext);

    // Progress the ongoing events
    ECONTEXT_LOCK(econtext);
    progress_econtext_sends(econtext);

    // Progress all active operations
    dpu_offload_status_t rc = progress_active_ops(econtext);
    if (rc)
    {
        ERR_MSG("progress_active_ops() failed");
    }
    ECONTEXT_UNLOCK(econtext);

    // Check for termination
    if (econtext->term.ev != NULL)
    {
        if (event_completed(econtext->term.ev))
        {
            event_return(&(econtext->term.ev));
            term_notification_completed(econtext);
        }
    }
    return;
}

static dpu_offload_status_t execution_context_init(offloading_engine_t *offload_engine, uint64_t type, execution_context_t **econtext)
{
    execution_context_t *ctx = DPU_OFFLOAD_MALLOC(sizeof(execution_context_t));
    CHECK_ERR_GOTO((ctx == NULL), error_out, "unable to allocate execution context");
    RESET_ECONTEXT(ctx);
#if OFFLOADING_MT_ENABLE
    ctx->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
#endif
    ctx->type = type;
    ctx->engine = offload_engine;
    ctx->progress = execution_context_progress;
    // scope_id is overwritten when the inter-service-processes connection manager sets inter-service-processes connections
    ctx->scope_id = SCOPE_HOST_DPU;
    ucs_list_head_init(&(ctx->ongoing_events));
    ucs_list_head_init(&(ctx->active_ops));
    *econtext = ctx;
    return DO_SUCCESS;
error_out:
    return DO_ERROR;
}

static void execution_context_fini(execution_context_t **ctx)
{
    if ((*ctx)->event_channels)
    {
        event_channels_fini(&((*ctx)->event_channels));
    }
    free(*ctx);
    *ctx = NULL;
}

/**
 * @brief client_init initialize the datastructure representing a client and perform the bootstrapping
 * procedure
 *
 * @param offload_engine offloading engine the client is associated with.
 * @param init_params Handle gathering all the initialization parameters that can be customized by the caller. Can be NULL to use defaults.
 * @return execution_context_t*
 */
execution_context_t *client_init(offloading_engine_t *offload_engine, init_params_t *init_params)
{
    execution_context_t *ctx = NULL;

    CHECK_ERR_GOTO((offload_engine == NULL), error_out, "Undefined handle");
    CHECK_ERR_GOTO((offload_engine->client != NULL), error_out, "offload engine already initialized as a client");

    // When calling this function, we know we can check whether we are
    // on a host or a DPU. If the process is running on a host, it is
    // therefore safe to initialize the engine's elements that are specific to
    // running on hosts.
    if (!offload_engine->on_dpu && !offload_engine->host_dpu_data_initialized)
        RESET_HOST_ENGINE(offload_engine);

    int rc = execution_context_init(offload_engine, CONTEXT_CLIENT, &ctx);
    CHECK_ERR_GOTO((rc != 0 || ctx == NULL), error_out, "execution_context_init() failed");
    if (init_params != NULL)
    {
        ctx->scope_id = init_params->scope_id;
        if (init_params->proc_info != NULL)
        {
            if (init_params->proc_info->group_uid != INT_MAX && init_params->proc_info->host_info == UINT64_MAX)
            {
                // We are in the context of an actual rank and the host UID was not set by the caller,
                // we set it for consistency and for future potential use
                host_info_t *host_info = NULL;
                host_info = DYN_ARRAY_GET_ELT(&(offload_engine->config->hosts_config),
                                              offload_engine->config->host_index,
                                              host_info_t);
                assert(host_info);
                init_params->proc_info->host_info = host_info->uid;
            }
            COPY_RANK_INFO(init_params->proc_info, &(ctx->rank));

            // In the context of inter-SP connections and situations without groups,
            // the group UID is set to INT_MAX and we should not try to update
            // the cache.
            if (ctx->rank.group_uid != INT_MAX)
            {
                /* Update the local cache for consistency */
                group_cache_t *gp_cache = GET_GROUP_CACHE_INTERNAL(&(offload_engine->procs_cache),
                                                                   ctx->rank.group_uid,
                                                                   ctx->rank.group_size);
                if (gp_cache->group_size <= 0)
                {
                    gp_cache->group_size = init_params->proc_info->group_size;
                    gp_cache->n_local_ranks = init_params->proc_info->n_local_ranks;
                    gp_cache->group_uid = init_params->proc_info->group_uid;
                }
            }
        }

        if (init_params->num_sps > 0 && offload_engine->num_service_procs == 0)
        {
            offload_engine->num_service_procs = init_params->num_sps;
        }
        if (init_params->num_sps > 0 && offload_engine->num_service_procs > 0 && init_params->num_sps != offload_engine->num_service_procs)
        {
            ERR_MSG("Inconsistent number of service processes: %ld vs. %ld", init_params->num_sps, offload_engine->num_service_procs);
            goto error_out;
        }
    }
    else
    {
        RESET_RANK_INFO(&(ctx->rank));
        ctx->rank.n_local_ranks = -1;
    }
    DBG("execution context successfully initialized (engine: %p)", ctx->engine);

    rc = client_init_context(ctx, init_params);
    CHECK_ERR_GOTO((rc), error_out, "init_client_context() failed");
    CHECK_ERR_GOTO((ctx->client == NULL), error_out, "client handle is undefined");
    DBG("client context successfully initialized (econtext=%p)", ctx);

    rc = event_channels_init(ctx);
    CHECK_ERR_GOTO((rc), error_out, "event_channels_init() failed");
    CHECK_ERR_GOTO((ctx->event_channels == NULL), error_out, "event channel object is undefined");
    ctx->client->event_channels = ctx->event_channels;
    ctx->client->event_channels->econtext = (struct execution_context *)ctx;
    DBG("event channels successfully initialized");

    switch (ctx->client->mode)
    {
    case UCX_LISTENER:
    {
        ucx_listener_client_connect(ctx->client);
        break;
    }
    default:
    {
        dpu_offload_status_t ret = oob_connect(ctx);
        CHECK_ERR_GOTO((ret), error_out, "oob_connect() failed");
    }
    }

    ECONTEXT_LOCK(ctx);
    rc = register_default_notifications(ctx->event_channels);
    ENGINE_LOCK(offload_engine);
    ADD_DEFAULT_ENGINE_CALLBACKS(offload_engine, ctx);
    ENGINE_UNLOCK(offload_engine);
    ECONTEXT_UNLOCK(ctx);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notfications() failed");

    // We add ourselves to the local EP cache as shadow service process
    if (ctx->rank.group_uid != INT_MAX &&
        ctx->rank.group_rank != INVALID_RANK &&
        !is_in_cache(&(offload_engine->procs_cache), ctx->rank.group_uid, ctx->rank.group_rank, ctx->rank.group_size))
    {
        dpu_offload_state_t ret;
        group_cache_t *gp_cache = NULL;
        assert(offload_engine->procs_cache.world_group == ctx->rank.group_uid);
        gp_cache = GET_GROUP_CACHE_INTERNAL(&(offload_engine->procs_cache),
                                            ctx->rank.group_uid,
                                            ctx->rank.group_size);
        assert(gp_cache);
        assert(gp_cache->persistent.num == 0); // The group is not in use yet so its seq num should be 0
        ctx->rank.group_seq_num = gp_cache->persistent.num = 1; // Now we mark it as in use so the seq num is set to 1
        ret = host_add_local_rank_to_cache(offload_engine, &(ctx->rank));
        CHECK_ERR_GOTO((ret != DO_SUCCESS), error_out, "host_add_local_rank_to_cache() failed (rc: %d)", ret);
    }

    return ctx;
error_out:
    if (offload_engine->client != NULL)
    {
        free(offload_engine->client);
        offload_engine->client = NULL;
    }
    return NULL;
}

void offload_engine_fini(offloading_engine_t **offload_engine)
{
    size_t i;
    assert(offload_engine);
    assert(*offload_engine);
    event_channels_fini(&((*offload_engine)->default_notifications));
    GROUPS_CACHE_FINI(&((*offload_engine)->procs_cache));
    DYN_LIST_FREE((*offload_engine)->free_op_descs, op_desc_t, item);
    DYN_LIST_FREE((*offload_engine)->free_cache_entry_requests, cache_entry_request_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_conn_params, conn_params_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_group_revoke_msgs_from_sps, group_revoke_msg_from_sp_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_group_revoke_msgs_from_ranks, group_revoke_msg_from_rank_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_pending_recv_group_add, pending_group_add_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_pending_send_group_add, pending_send_group_add_t, item);
    DYN_LIST_FREE((*offload_engine)->pool_pending_recv_cache_entries, pending_recv_cache_entry_t, item);
    DYN_LIST_FREE((*offload_engine)->free_sp_cache_hash_obj, sp_cache_data_t, item);
    DYN_LIST_FREE((*offload_engine)->free_host_cache_hash_obj, host_cache_data_t, item);
    for (i = 0; i < (*offload_engine)->config->num_dpus; i++)
    {
        remote_dpu_info_t *remote_dpu = NULL;
        remote_dpu = DYN_ARRAY_GET_ELT(&((*offload_engine)->dpus), i, remote_dpu_info_t);
        assert(remote_dpu);
        DYN_ARRAY_FREE(&(remote_dpu->local_service_procs));
    }
    DYN_ARRAY_FREE(&((*offload_engine)->dpus));
    if ((*offload_engine)->on_dpu)
    {
        DYN_ARRAY_FREE(GET_ENGINE_LIST_SERVICE_PROCS((*offload_engine)));
    }

    assert((*offload_engine)->self_econtext);
#if !USE_AM_IMPLEM
    if (!(*offloading_engine)->settings.ucx_am_backend_enabled)
    {
        // Before finalizing the self execution context, clean up the pending recvs for notifications
        if ((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.req != NULL)
        {
            ucp_request_cancel(GET_WORKER((*offload_engine)->self_econtext),
                               (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.req);
            ucp_request_release((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.req);
            (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.req = NULL;
        }
        if ((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.req != NULL)
        {
            ucp_request_cancel(GET_WORKER((*offload_engine)->self_econtext),
                               (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.req);
            ucp_request_release((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.req);
            (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.req = NULL;
        }
        if ((*offload_engine)->settings.buddy_buffer_system_enabled)
        {
            if ((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf != NULL)
            {
                SMART_BUFF_RETURN(&((*offload_engine)->self_econtext->engine->smart_buffer_sys),
                                  (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.hdr.payload_size,
                                  (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf);
                (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.smart_buf = NULL;
            }
        }
        else
        {
            if ((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.buffer != NULL)
            {
                free((*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.buffer);
                (*offload_engine)->self_econtext->event_channels->notif_recv.ctx.payload_ctx.buffer = NULL;
            }
        }
    }
#endif // !USE_AM_IMPLEM

    execution_context_fini(&((*offload_engine)->self_econtext));

    if ((*offload_engine)->client)
    {
        client_fini(&((*offload_engine)->client));
    }

    for (i = 0; i < (*offload_engine)->num_servers; i++)
    {
        // server_fini() FIXME
    }
    for (i = 0; i < (*offload_engine)->num_inter_service_proc_clients; i++)
    {
        client_fini(&((*offload_engine)->inter_service_proc_clients[i].client_econtext));
    }

    if ((*offload_engine)->config != NULL)
    {
        offload_config_free((*offload_engine)->config);
        // engine->config is usually not allocate with malloc, no need to free it here
    }

    if ((*offload_engine)->ucp_worker_allocated && (*offload_engine)->ucp_worker)
    {
        ucp_worker_destroy((*offload_engine)->ucp_worker);
        (*offload_engine)->ucp_worker = NULL;
    }

    if ((*offload_engine)->ucp_context_allocated && (*offload_engine)->ucp_context)
    {
        ucp_cleanup((*offload_engine)->ucp_context);
        (*offload_engine)->ucp_context = NULL;
    }

    if ((*offload_engine)->settings.buddy_buffer_system_enabled)
    {
        SMART_BUFFS_FINI(&((*offload_engine)->smart_buffer_sys));
    }

    if ((*offload_engine)->settings.ucx_am_backend_enabled)
    {
        uint64_t key;
        execution_context_t *value;
        kh_foreach((*offload_engine)->client_lookup_table,
                   key,
                   value,
                   {/* nothing special to do*/})
            kh_destroy(client_lookup_hash_t, (*offload_engine)->client_lookup_table);

        for (i = 0; i < (*offload_engine)->free_pending_rdv_recv->capacity; i++)
        {
            pending_am_rdv_recv_t *elt;
            DYN_LIST_GET((*offload_engine)->free_pending_rdv_recv, pending_am_rdv_recv_t, item, elt);
            if (elt == NULL)
                continue;
            if (elt->buff_size > 0)
            {
                if (elt->user_data != NULL)
                {
                    if (elt->pool.mem_pool != NULL && elt->pool.return_buf != NULL)
                    {
                        elt->pool.return_buf(elt->pool.mem_pool, elt->user_data);
                        RESET_NOTIF_INFO(&(elt->pool));
                    }
                    else
                    {
                        if (elt->user_data != NULL)
                            free(elt->user_data);
                    }
                    elt->user_data = NULL;
                }
                elt->buff_size = 0;
            }
        }
        DYN_LIST_FREE((*offload_engine)->free_pending_rdv_recv, pending_am_rdv_recv_t, item);
    }

    if ((*offload_engine)->servers)
    {
        free((*offload_engine)->servers);
        (*offload_engine)->servers = NULL;
    }
    if ((*offload_engine)->inter_service_proc_clients)
    {
        free((*offload_engine)->inter_service_proc_clients);
        (*offload_engine)->inter_service_proc_clients = NULL;
    }

    free(*offload_engine);
    *offload_engine = NULL;
}

void client_fini(execution_context_t **exec_ctx)
{
    execution_context_t *context;
    dpu_offload_status_t rc;
    dest_client_t dest_info;

    if (exec_ctx == NULL || *exec_ctx == NULL)
        return;

    context = *exec_ctx;
    if (context->type != CONTEXT_CLIENT)
    {
        ERR_MSG("invalid type: %d", context->type);
        return;
    }

    DBG("Sending termination message to associated server...");
    dest_info.id = context->client->server_id;
    dest_info.ep = context->client->server_ep;
    rc = send_term_msg(context, &dest_info);
    if (rc != DO_SUCCESS)
    {
        ERR_MSG("send_term_msg() failed");
        return;
    }
    assert(context->term.ev);
    DBG("Termination message successfully emitted");

    // Loop until the term message completes
    while (context->client->done == false)
    {
        // We explicitly manage the event so we need to progress it here
        context->progress(context);
    }

    // Some memory is freed in the term_notification_completed callback

    // The event system is finalized when we finalize the execution context object.
    // The worker is freed when the engine is finalized.

    free((*exec_ctx)->client);
    (*exec_ctx)->client = NULL;

    execution_context_fini(exec_ctx);
}

static void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucx_server_ctx_t *context = arg;
    ucp_conn_request_attr_t attr;
    // char ip_str[IP_STRING_LEN];
    // char port_str[PORT_STRING_LEN];
    ucs_status_t status;
    DBG("Connection handler invoked");
    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK)
    {
        // DBG("Server received a connection request from client at address %s:%s", ip_str, port_str);
    }
    else if (status != UCS_ERR_UNSUPPORTED)
    {
        DBG("failed to query the connection request (%s)", ucs_status_string(status));
    }
    if (context->conn_request == NULL)
    {
        context->conn_request = conn_request;
    }
    else
    {
        /* The server is already handling a connection request from a client,
         * reject this new one */
        DBG("Rejecting a connection request. Only one client at a time is supported.\n");
        status = ucp_listener_reject(context->listener, conn_request);
        if (status != UCS_OK)
        {
            ERR_MSG("server failed to reject a connection request: (%s)", ucs_status_string(status));
        }
    }
}

static dpu_offload_status_t ucx_listener_server(dpu_offload_server_t *server)
{
    ucp_listener_params_t params = {0};
    ucp_listener_attr_t attr;
    ucs_status_t status;
    // char *port_str;
    set_sock_addr(server->conn_params.addr_str, server->conn_params.port, &(server->conn_params.saddr));
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                        UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr = (const struct sockaddr *)&(server->conn_params.saddr);
    params.sockaddr.addrlen = sizeof(server->conn_params.saddr);
    params.conn_handler.cb = server_conn_handle_cb;
    params.conn_handler.arg = &(server->conn_data.ucx_listener.context);
    /* Create a listener on the server side to listen on the given address.*/
    // DBG("Creating listener on %s:%s", server->conn_params.addr_str, port_str);
    status = ucp_listener_create(GET_WORKER(server->econtext), &params, &(server->conn_data.ucx_listener.context.listener));
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "failed to listen (%s)", ucs_status_string(status));

    /* Query the created listener to get the port it is listening on. */
    attr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
    status = ucp_listener_query(server->conn_data.ucx_listener.context.listener, &attr);
    if (status != UCS_OK)
    {
        ERR_MSG("failed to query the listener (%s)", ucs_status_string(status));
        ucp_listener_destroy(server->conn_data.ucx_listener.context.listener);
        return DO_ERROR;
    }
    // DBG("server is listening on IP %s port %s", server->conn_params.addr_str, port_str);
    return DO_SUCCESS;
}

#if 0
static void oob_recv_addr_handler(void *request, ucs_status_t status,
                                  ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *)request;
    context->completed = 1;

    DBG("receive handler for addr called with status %d (%s), length %lu",
        status, ucs_status_string(status), info->length);
}
#endif

#if 0
static void oob_recv_rank_handler(void *request, ucs_status_t status,
                                  ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *)request;
    context->completed = 1;

    DBG("receive handler for rank called with status %d (%s), length %lu",
        status, ucs_status_string(status), info->length);
}
#endif

// This function assumes the execution context is properly locked before it is invoked
static inline uint64_t generate_unique_client_id(execution_context_t *econtext)
{
    // For now we only use the slot that the client will have in the list of connected clients
    return (uint64_t)(econtext->server->connected_clients.num_total_connected_clients + econtext->server->connected_clients.num_ongoing_connections);
}

/**
 * @brief Note that this function assumes the execution context is NOT locked before it is invoked.
 *
 * @param econtext
 * @return dpu_offload_status_t
 */
static dpu_offload_status_t oob_server_listen(execution_context_t *econtext)
{
    /* OOB connection establishment */
    char client_addr[INET_ADDRSTRLEN];
    dpu_offload_status_t rc = oob_server_accept(econtext, client_addr);
    CHECK_ERR_RETURN((rc), DO_ERROR, "oob_server_accept() failed");
    ECONTEXT_LOCK(econtext);
    DBG("Sending my worker's data...\n");
    ssize_t size_sent = send(econtext->server->conn_data.oob.sock,
                             &(econtext->server->conn_data.oob.local_addr_len),
                             sizeof(econtext->server->conn_data.oob.local_addr_len),
                             0);
    DBG("Addr length sent (len: %ld)", size_sent);
    assert(size_sent == sizeof(econtext->server->conn_data.oob.local_addr_len));
    size_sent = send(econtext->server->conn_data.oob.sock,
                     econtext->server->conn_data.oob.local_addr,
                     econtext->server->conn_data.oob.local_addr_len,
                     0);
    DBG("Address sent (len: %ld)", size_sent);
    assert(size_sent == econtext->server->conn_data.oob.local_addr_len);
    size_sent = send(econtext->server->conn_data.oob.sock,
                     &(econtext->server->id),
                     sizeof(econtext->server->id),
                     0);
    DBG("Server ID sent (len: %ld, id: %" PRIu64 ")", size_sent, econtext->server->id);
    if (econtext->engine->on_dpu)
    {
        size_sent = send(econtext->server->conn_data.oob.sock,
                         &(econtext->engine->config->local_service_proc.info.global_id),
                         sizeof(econtext->engine->config->local_service_proc.info.global_id),
                         0);
        assert(size_sent == sizeof(econtext->engine->config->local_service_proc.info.global_id));
    }
    else
    {
        uint64_t val = UINT64_MAX;
        size_sent = send(econtext->server->conn_data.oob.sock,
                         &val,
                         sizeof(econtext->engine->config->local_service_proc.info.global_id),
                         0);
        assert(size_sent == sizeof(econtext->engine->config->local_service_proc.info.global_id));
    }
    DBG("Global ID sent (len: %ld)", size_sent);
    uint64_t client_id = generate_unique_client_id(econtext);
#if !NDEBUG
    if (econtext->engine->on_dpu && econtext->scope_id == SCOPE_INTER_SERVICE_PROCS)
    {
        assert(client_id < econtext->engine->num_service_procs);
    }
#endif
    size_sent = send(econtext->server->conn_data.oob.sock, &client_id, sizeof(uint64_t), 0);
    DBG("Client ID sent (len: %ld, value: %" PRIu64 ")", size_sent, client_id);
    assert(size_sent == sizeof(uint64_t));

    /* All done, the rest is done when progressing the execution contexts */
    peer_info_t *client_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), client_id, peer_info_t);
    assert(client_info);
    client_info->bootstrapping.phase = OOB_CONNECT_DONE;
    client_info->id = client_id;
    client_info->peer_addr_str = client_addr;
    econtext->server->connected_clients.num_ongoing_connections++;
    DBG("Client #%" PRIu64 " (%p) is now in the OOB_CONNECT_DONE state", client_id, client_info);
    DBG("Total number of ongoing connections: %ld\n", econtext->server->connected_clients.num_ongoing_connections);
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
}

static void *connect_thread(void *arg)
{
    execution_context_t *econtext = (execution_context_t *)arg;
    if (econtext == NULL)
    {
        ERR_MSG("Execution context is NULL");
        pthread_exit(NULL);
    }

    ECONTEXT_LOCK(econtext);
    bool done = econtext->server->done;
    int mode = econtext->server->mode;
    ECONTEXT_UNLOCK(econtext);
    while (!done)
    {
        switch (mode)
        {
        case UCX_LISTENER:
        {
            ucx_listener_server(econtext->server);
            DBG("Waiting for connection on UCX listener...");
            while (econtext->server->conn_data.ucx_listener.context.conn_request == NULL)
            {
                DBG("Progressing worker...");
                ucp_worker_progress(GET_WORKER(econtext));
            }

            if (econtext->server->connected_cb != NULL)
            {
                DBG("Invoking connection completion callback...");
                econtext->server->connected_cb(NULL);
            }

            break;
        }
        default:
        {
            // Note: the connection completion callback is called at the end of oob_server_listen()
            int rc = oob_server_listen(econtext);
            if (rc)
            {
                ERR_MSG("oob_server_listen() failed");
                pthread_exit(NULL);
            }
        }
        }

        ECONTEXT_LOCK(econtext);
        done = econtext->server->done;
        ECONTEXT_UNLOCK(econtext);
    }

    pthread_exit(NULL);
}

static dpu_offload_status_t start_server(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");
    CHECK_ERR_RETURN((econtext->server == NULL), DO_ERROR, "undefined server handle");

    int rc = pthread_create(&econtext->server->connect_tid, NULL, &connect_thread, econtext);
    CHECK_ERR_RETURN((rc), DO_ERROR, "unable to start connection thread");
    return DO_SUCCESS;
}

#define MAX_CACHE_ENTRIES_PER_PROC (8)
dpu_offload_status_t server_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    int ret;
    size_t i;
    econtext->type = CONTEXT_SERVER;
    econtext->server = DPU_OFFLOAD_MALLOC(sizeof(dpu_offload_server_t));
    CHECK_ERR_RETURN((econtext->server == NULL), DO_ERROR, "Unable to allocate server handle");
    RESET_SERVER(econtext->server);
    econtext->server->econtext = (struct execution_context *)econtext;
    econtext->server->mode = OOB; // By default, we connect with the OOB mode
    DYN_ARRAY_ALLOC(&(econtext->server->connected_clients.clients), DEFAULT_MAX_NUM_CLIENTS, peer_info_t);
    for (i = 0; i < DEFAULT_MAX_NUM_CLIENTS; i++)
    {
        peer_info_t *peer_info = DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), i, peer_info_t);
        assert(peer_info);
        peer_info->bootstrapping.phase = BOOTSTRAP_NOT_INITIATED;
        peer_info->bootstrapping.addr_ctx.complete = false;
        peer_info->bootstrapping.addr_size_ctx.complete = false;
        peer_info->bootstrapping.rank_ctx.complete = false;
#if USE_AM_IMPLEM
        peer_info->ctx.complete = false;
#else
        peer_info->notif_recv.ctx.complete = true; // to make sure we can post the initial recv
#endif
        // TODO: Not sure this is used, but leaving for now. Where is the memory freed?
        DYN_ARRAY_ALLOC(&(peer_info->cache_entry_index_data), MAX_CACHE_ENTRIES_PER_PROC, peer_cache_entry_index_t);
    }

    if (init_params == NULL || init_params->conn_params == NULL)
    {
        DBG("no initialization parameters specified, try to gather parameters from environment...");
        ret = get_env_config(econtext->engine, &(econtext->server->conn_params));
        CHECK_ERR_RETURN((ret), DO_ERROR, "get_env_config() failed");
    }
    else
    {
        DBG("using connection parameters that have been passed in (init_params=%p, conn_params=%p, econtext=%p, econtext->conn_params=%p, addr=%s)",
            init_params, init_params->conn_params, econtext, &(econtext->server->conn_params), init_params->conn_params->addr_str);
        econtext->server->conn_params.addr_str = init_params->conn_params->addr_str;
        econtext->server->conn_params.port = init_params->conn_params->port;
        econtext->server->conn_params.port_str = NULL;
        econtext->scope_id = init_params->scope_id;
        if (init_params->id_set)
            econtext->server->id = init_params->id;
    }

    if (init_params != NULL && init_params->connected_cb != NULL)
    {
        econtext->server->connected_cb = init_params->connected_cb;
    }

#if OFFLOADING_MT_ENABLE
    econtext->server->mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
#endif

    // Make sure we correctly handle whether the UCX context/worker is provided
    DO_INIT_WORKER(econtext, init_params);
    assert(GET_WORKER(econtext));

    switch (econtext->server->mode)
    {
    case UCX_LISTENER:
    {
        econtext->server->conn_data.ucx_listener.context.conn_request = NULL;
        break;
    }
    default:
    {
        // OOB
        DBG("server %" PRIu64 " initialized with OOB backend (econtext: %p, scope_id: %d, engine: %p)",
            econtext->server->id, econtext, econtext->scope_id, econtext->engine);
        econtext->server->conn_data.oob.tag = OOB_DEFAULT_TAG;
        econtext->server->conn_data.oob.tag_mask = UINT64_MAX;
        econtext->server->conn_data.oob.addr_msg_str = strdup(UCX_ADDR_MSG); // fixme: correctly free
        econtext->server->conn_data.oob.peer_addr = NULL;
        econtext->server->conn_data.oob.local_addr = NULL;
        econtext->server->conn_data.oob.local_addr_len = 0;
        econtext->server->conn_data.oob.peer_addr_len = 0;
        econtext->server->conn_data.oob.listenfd = -1;
        ucp_worker_h worker = GET_WORKER(econtext);
        assert(worker);
        ucs_status_t status = ucp_worker_get_address(worker,
                                                     &(econtext->server->conn_data.oob.local_addr),
                                                     &(econtext->server->conn_data.oob.local_addr_len));
        CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_worker_get_address() failed");
    }
    }

    return DO_SUCCESS;
}

#if 0
static ucs_status_t server_create_ep(ucp_worker_h data_worker,
                                     ucp_conn_request_h conn_request,
                                     ucp_ep_h *server_ep)
{
    ucp_ep_params_t ep_params = {0};
    ucs_status_t status;
    /* Server creates an ep to the client on the data worker.
     * This is not the worker the listener was created on.
     * The client side should have initiated the connection, leading
     * to this ep's creation */
    ep_params.field_mask = UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request = conn_request;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
    status = ucp_ep_create(data_worker, &ep_params, server_ep);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "failed to create an endpoint on the server: (%s)",
                     ucs_status_string(status));
    return status;
}
#endif

/**
 * @brief Initialize a connection server.
 *
 * @param offloading_engine Associated offloading engine.
 * @param conn_params Connection parameters, e.g., port to use to listen for connections. If NULL, the configuration will be pulled from environment variables.
 * @return execution_context_t*
 */
execution_context_t *server_init(offloading_engine_t *offloading_engine, init_params_t *init_params)
{
    CHECK_ERR_GOTO((offloading_engine == NULL), error_out, "Handle is NULL");

    execution_context_t *execution_context;
    DBG("initializing execution context...");
    dpu_offload_status_t rc = execution_context_init(offloading_engine, CONTEXT_SERVER, &execution_context);
    CHECK_ERR_GOTO((rc), error_out, "execution_context_init() failed");
    DBG("execution context created (econtext: %p, scope_id: %d)", execution_context, execution_context->scope_id);

    DBG("initializing server context for econtext %p (scope_id=%d)...", execution_context, execution_context->scope_id);
    rc = server_init_context(execution_context, init_params);
    CHECK_ERR_GOTO((rc), error_out, "server_init_context() failed");
    CHECK_ERR_GOTO((execution_context->server == NULL), error_out, "undefined server handle");
    DBG("server handle %p successfully created (worker=%p, econtext=%p, scope_id=%u)",
        execution_context->server,
        GET_WORKER(execution_context),
        execution_context,
        execution_context->scope_id);
    // Note: on DPUs, for inter-service-processes communications, we want the server to have the unique global ID based on
    // the configuration, the same for client service processes. So when a client service process sends a message to a server
    // service process, we can create a unique tag that identify the server-client connection.
    if (offloading_engine->on_dpu && init_params != NULL && init_params->scope_id == SCOPE_INTER_SERVICE_PROCS)
        execution_context->server->id = offloading_engine->config->local_service_proc.info.global_id;
    else if (init_params != NULL && init_params->id_set)
        execution_context->server->id = init_params->id;
    else
        execution_context->server->id = offloading_engine->num_servers;
    offloading_engine->servers[offloading_engine->num_servers] = execution_context;
    offloading_engine->num_servers++;

    rc = event_channels_init(execution_context);
    CHECK_ERR_GOTO((rc), error_out, "event_channels_init() failed");
    CHECK_ERR_GOTO((execution_context->event_channels == NULL), error_out, "event channel handle is undefined");
    DBG("event channel %p successfully initialized (econtext: %p, scope_id: %d)",
        execution_context->event_channels,
        execution_context,
        execution_context->scope_id);
    execution_context->server->event_channels = execution_context->event_channels;
    execution_context->server->event_channels->econtext = (struct execution_context *)execution_context;

    /* Start the server to accept connections */
    ucs_status_t status = start_server(execution_context);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "start_server() failed");

#if !NDEBUG
    if (init_params != NULL && init_params->conn_params != NULL)
    {
        DBG("Server initiated on %s:%d (econtext: %p, scope_id=%d)",
            init_params->conn_params->addr_str,
            init_params->conn_params->port,
            execution_context,
            execution_context->scope_id);
        assert(init_params->conn_params->addr_str);
        assert(init_params->conn_params->port > 0);
    }
#endif

    ECONTEXT_LOCK(execution_context);
    dpu_offload_ev_sys_t *ev_sys = execution_context->event_channels;
    rc = register_default_notifications(ev_sys);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notifications() failed");

    ENGINE_LOCK(offloading_engine);
    ADD_DEFAULT_ENGINE_CALLBACKS(offloading_engine, execution_context);
    ENGINE_UNLOCK(offloading_engine);
    ECONTEXT_UNLOCK(execution_context);

    return execution_context;

error_out:
    ECONTEXT_UNLOCK(execution_context);
    ENGINE_UNLOCK(offloading_engine);
    if (execution_context != NULL)
    {
        free(execution_context);
    }
    return NULL;
}

void server_fini(execution_context_t **exec_ctx)
{
    size_t i;
    if (exec_ctx == NULL || *exec_ctx == NULL)
        return;

    execution_context_t *context = *exec_ctx;
    if (context->type != CONTEXT_SERVER)
    {
        ERR_MSG("invalid context");
        return;
    }

    dpu_offload_server_t *server = (*exec_ctx)->server;
    pthread_cancel(server->connect_tid);
#if OFFLOADING_MT_ENABLE
    pthread_mutex_destroy(&(server->mutex));
#endif

    /* Close the clients' endpoint */
    for (i = 0; i < server->connected_clients.num_connected_clients; i++)
    {
        peer_info_t *peer_info = DYN_ARRAY_GET_ELT(&(server->connected_clients.clients), i, peer_info_t);
        assert(peer_info);
        if (peer_info->ep != NULL)
        {
            ep_close(GET_WORKER(*exec_ctx), peer_info->ep);
            peer_info->ep = NULL;
        }
    }

    switch (server->mode)
    {
    case UCX_LISTENER:
    {
        server->conn_data.ucx_listener.context.conn_request = NULL;
        ucp_listener_destroy(server->conn_data.ucx_listener.context.listener);
        break;
    }
    default:
        /* OOB */
        ucp_worker_release_address(GET_WORKER(*exec_ctx), server->conn_data.oob.local_addr);
        if (server->conn_data.oob.peer_addr != NULL)
        {
            free(server->conn_data.oob.peer_addr);
            server->conn_data.oob.peer_addr = NULL;
        }
    }

    // The event system is freed when the execution context object is finalized
    // The worker is freed when the engine is finalized

    free(context->server);
    context->server = NULL;

    execution_context_fini(&context);
}
