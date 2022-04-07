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
#include "dpu_offload_comm_channels.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_envvars.h"
#include "dynamic_structs.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_ops.h"
#include "dpu_offload_comms.h"

extern dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *);
#if USE_AM_IMPLEM
extern int am_send_event_msg(dpu_offload_event_t **event);
#else
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

#define GET_PEER_DATA_HANDLE(_pool_free_peer_descs, _peer_data) \
    DYN_LIST_GET(_pool_free_peer_descs, peer_cache_entry_t, item, _peer_data);

extern dpu_offload_status_t get_env_config(conn_params_t *params);

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    fprintf(stderr, "error handling callback was invoked with status %d (%s)\n",
            status, ucs_status_string(status));
}

static void oob_recv_addr_handler_2(void *request, ucs_status_t status, const ucp_tag_recv_info_t *tag_info, void *user_data)
{
    DBG("Bootstrapping message received\n");
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
    struct oob_msg *msg = NULL;

    size_t msg_len = 0;
    ucp_request_param_t send_param;
    ucp_tag_recv_info_t info_tag;
    ucs_status_t status;
    ucp_tag_message_h msg_tag;
    int ret;

    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");
    ECONTEXT_LOCK(econtext);

    dpu_offload_server_t *server = econtext->server;
    CHECK_ERR_GOTO((server == NULL), error_out, "server handle is undefined");

    /* 1. Receive the address size */
    size_t client_addr_size = 0;
    ucp_tag_t ucp_tag, ucp_tag_mask;
    DBG("Tag: %d\n", econtext->server->conn_data.oob.tag);
    MAKE_RECV_TAG(ucp_tag, ucp_tag_mask, econtext->server->conn_data.oob.tag, 0, 0, 0, 0);
    ucp_request_param_t recv_param;
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.addr_size_ctx);
    recv_param.cb.recv = oob_recv_addr_handler_2;
    assert(client_info->bootstrapping.addr_size_ctx.complete == false);
    client_info->bootstrapping.addr_size_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                                    &(client_info->peer_addr_len),
                                                                    sizeof(size_t),
                                                                    ucp_tag,
                                                                    ucp_tag_mask,
                                                                    &recv_param);
    if (client_info->bootstrapping.addr_size_request == NULL)
    {
        DBG("We received the address size right away, callback not invoked, dealing directly with it");
        client_info->bootstrapping.addr_size_ctx.complete = true;
    }
    else
    {
        DBG("Reception of the address size still in progress");
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
    ucp_request_param_t recv_param;
    client_info->peer_addr = MALLOC(client_info->peer_addr_len);
    CHECK_ERR_GOTO((client_info->peer_addr == NULL),
                   error_out,
                   "unable to allocate memory for peer address (%ld bytes)",
                   client_info->peer_addr_len);
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.addr_ctx);
    recv_param.cb.recv = oob_recv_addr_handler_2;
    MAKE_RECV_TAG(ucp_tag, ucp_tag_mask, econtext->server->conn_data.oob.tag, 0, 0, 0, 0);
    client_info->bootstrapping.addr_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                               client_info->peer_addr,
                                                               client_info->peer_addr_len,
                                                               ucp_tag,
                                                               ucp_tag_mask,
                                                               &recv_param);
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
    ucp_ep_params_t ep_params;
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
    ep_params.address = client_info->peer_addr;
    ep_params.user_data = &(client_info->ep_status);
    ucp_worker_h worker = GET_WORKER(econtext);
    CHECK_ERR_GOTO((worker == NULL), error_out, "undefined worker");
    ucp_ep_h client_ep;
    ucs_status_t status = ucp_ep_create(worker, &ep_params, &client_ep);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "ucp_ep_create() failed: %s", ucs_status_string(status));
    client_info->ep = client_ep;
    DBG("endpoint successfully created");

    // receive the rank/group data
    DBG("Ready to receive peer data\n");
    ucp_tag_t ucp_tag, ucp_tag_mask;
    ucp_request_param_t recv_param;
    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    recv_param.datatype = ucp_dt_make_contig(1);
    recv_param.user_data = &(client_info->bootstrapping.rank_ctx);
    recv_param.cb.recv = oob_recv_addr_handler_2;
    MAKE_RECV_TAG(ucp_tag, ucp_tag_mask, econtext->server->conn_data.oob.tag, 0, 0, 0, 0);
    client_info->bootstrapping.rank_request = ucp_tag_recv_nbx(GET_WORKER(econtext),
                                                               &(client_info->rank_data),
                                                               sizeof(rank_info_t),
                                                               ucp_tag,
                                                               ucp_tag_mask,
                                                               &recv_param);
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
                    fprintf(stderr, "Not in presentation format\n");
                else
                    fprintf(stderr, "inet_pton() failed\n");
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
        fprintf(stderr, "Invalid address family");
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
    int connfd;
    struct sockaddr_in servaddr;
    int optval = 1;
    int rc;

    ECONTEXT_LOCK(econtext);
    uint16_t server_port = econtext->server->conn_params.port;

    if (econtext->server->conn_data.oob.listenfd == -1)
    {
        int accept_sock;
        econtext->server->conn_data.oob.listenfd = socket(AF_INET, SOCK_STREAM, 0);

        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port = htons(server_port);

        setsockopt(econtext->server->conn_data.oob.listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        rc = bind(econtext->server->conn_data.oob.listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
        CHECK_ERR_GOTO((rc), error_out, "bind() failed: %s", strerror(errno));
        rc = listen(econtext->server->conn_data.oob.listenfd, 1024);
        CHECK_ERR_GOTO((rc), error_out, "listen() failed: %s", strerror(errno));

        // fixme: debug when multi-connections are required
        DBG("Accepting connection on port %" PRIu16 "...", server_port);
        struct sockaddr_in addr;
        int addr_len = sizeof(client_addr);
        ECONTEXT_UNLOCK(econtext);
        accept_sock = accept(econtext->server->conn_data.oob.listenfd, (struct sockaddr *)&addr, &addr_len);
        ECONTEXT_LOCK(econtext);
        econtext->server->conn_data.oob.sock = accept_sock;
        struct in_addr ipAddr = addr.sin_addr;
        inet_ntop(AF_INET, &ipAddr, client_addr, INET_ADDRSTRLEN);
        DBG("Connection accepted from %s on fd=%d", client_addr, econtext->server->conn_data.oob.sock);
    }
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;

error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

#define MAX_RETRY (300) // Never try to connect for more than 5 minutes

// This function assumes the associated execution context is properly locked before it is invoked
dpu_offload_status_t oob_client_connect(dpu_offload_client_t *client, sa_family_t af)
{
    int listenfd = -1;
    int optval = 1;
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

    bool connected = false;
    int retry = 1;
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
                int conn_port;
                conn_port = ntohs(conn_addr.sin_port);
                DBG("Connection established, fd = %d, addr=%s:%d", client->conn_data.oob.sock, conn_ip, conn_port);
                break;
            }
            else
            {
                retry *= 2;
                DBG("Connection to %s:%d failed (%s), retrying in %d seconds...",
                    client->conn_params.addr_str,
                    client->conn_params.port,
                    strerror(errno),
                    retry);
                sleep(retry);
            }
        } while (rc != 0 && retry < MAX_RETRY);
        CHECK_ERR_GOTO((rc != 0), err_close_sockfd, "Connection failed (rc: %d)", rc);
    }

    CHECK_ERR_GOTO((client->conn_data.oob.sock < 0), err_close_sockfd, "Unable to connect to server: invalid file descriptor (%d)", client->conn_data.oob.sock);

out_free_res:
    freeaddrinfo(res);
out:
    return rc;
err_close_sockfd:
    close(client->conn_data.oob.sock);
    rc = DO_ERROR;
    goto out_free_res;
}

static void ep_close(ucp_worker_h ucp_worker, ucp_ep_h ep)
{
    ucp_request_param_t param;
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
err:
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

#define DO_INIT_WORKER(_econtext, _init_params)                                                                    \
    do                                                                                                             \
    {                                                                                                              \
        ucp_worker_h _econtext_worker;                                                                             \
        if ((_init_params) == NULL || (_init_params)->worker == NULL)                                              \
        {                                                                                                          \
            if ((_econtext)->engine->ucp_context == NULL)                                                          \
            {                                                                                                      \
                (_econtext)->engine->ucp_context = INIT_UCX();                                                     \
            }                                                                                                      \
            if ((_econtext)->engine->ucp_worker == NULL)                                                           \
            {                                                                                                      \
                dpu_offload_status_t _ret = init_context(&((_econtext)->engine->ucp_context),                      \
                                                         &(_econtext_worker));                                     \
                CHECK_ERR_RETURN((_ret != 0), DO_ERROR, "init_context() failed");                                  \
                SET_WORKER((_econtext), _econtext_worker);                                                         \
                DBG("context successfully initialized (worker=%p)", _econtext_worker);                             \
            }                                                                                                      \
        }                                                                                                          \
        if ((_init_params) != NULL && (_init_params)->worker != NULL)                                              \
        {                                                                                                          \
            DBG("re-using UCP worker for new execution context");                                                  \
            /* Notes: the context usually imposes different requirements:                                       */ \
            /* - on DPUs, we know there is no external UCX context and worker so they are initialized early on, */ \
            /*   during the initialization of the engine so we can establish connections to other DPUs and set  */ \
            /*   the self endpoint that is required to maintain the endpoint cache.                             */ \
            /* - on the host, it depends on the calling code: if the UCX context and worker are available when  */ \
            /*   the engine is initialized, they can be passed in then; but in other cases like UCC, they are   */ \
            /*   not available at the time and set during the creation of the execution contexts.               */ \
            if ((_econtext)->engine->ucp_context == NULL)                                                          \
                (_econtext)->engine->ucp_context = (_init_params)->ucp_context;                                    \
            SET_WORKER(_econtext, (_init_params)->worker);                                                         \
        }                                                                                                          \
    } while (0)

dpu_offload_status_t client_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    int ret;
    dpu_offload_status_t rc;
    econtext->type = CONTEXT_CLIENT;
    econtext->client = MALLOC(sizeof(dpu_offload_client_t));
    CHECK_ERR_RETURN((econtext->client == NULL), DO_ERROR, "Unable to allocate client handle\n");
    econtext->client->econtext = (struct execution_context *)econtext;
    econtext->client->connected_cb = NULL;
    econtext->client->bootstrapping.phase = BOOTSTRAP_NOT_INITIATED;
    ret = pthread_mutex_init(&(econtext->client->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));

    // If the connection parameters were not passed in, we get everything using environment variables
    if (init_params == NULL || init_params->conn_params == NULL)
    {
        rc = get_env_config(&(econtext->client->conn_params));
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
    if (init_params != NULL && init_params->proc_info != NULL)
    {
        econtext->rank.group_id = init_params->proc_info->group_id;
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
    ucp_ep_params_t ep_params;
    ep_params.field_mask = UCP_EP_PARAM_FIELD_FLAGS |
                           UCP_EP_PARAM_FIELD_SOCK_ADDR |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
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

static void oob_send_cb(void *request, ucs_status_t status, void *ctx)
{
    struct ucx_context *context = (struct ucx_context *)request;
    const char *str = (const char *)ctx;

    context->completed = 1;

    DBG("send handler called for \"%s\" with status %d (%s)", str, status, ucs_status_string(status));
}

static void ucx_client_boostrap_send_cb(void *request, ucs_status_t status, void *user_data)
{  
    am_req_t *ctx = (am_req_t *)user_data;
    ctx->complete = 1;
}

static dpu_offload_status_t client_ucx_boostrap_step3(execution_context_t *econtext)
{
    ECONTEXT_LOCK(econtext);
    DBG("Sent addr to server");
    // We send our "rank", i.e., unique application ID
    DBG("Sending my group/rank info (%" PRId64 "/%" PRId64 "), len=%ld", econtext->rank.group_id, econtext->rank.group_rank, sizeof(econtext->rank));
    struct ucx_context *rank_request = NULL;
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_boostrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.rank_ctx);
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag, 0, 0, 0, 0);
    econtext->client->bootstrapping.rank_request = ucp_tag_send_nbx(econtext->client->server_ep,
                                                                    &(econtext->rank),
                                                                    sizeof(rank_info_t),
                                                                    ucp_tag,
                                                                    &send_param);
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
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_boostrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.addr_ctx);
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag, 0, 0, 0, 0);
    econtext->client->bootstrapping.addr_request = ucp_tag_send_nbx(econtext->client->server_ep,
                                                                    econtext->client->conn_data.oob.local_addr,
                                                                    econtext->client->conn_data.oob.local_addr_len,
                                                                    ucp_tag,
                                                                    &send_param);
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
    ucp_ep_params_t ep_params;
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address = econtext->client->conn_data.oob.peer_addr;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data = &(econtext->client->server_ep_status);

    ucs_status_t status = ucp_ep_create(GET_WORKER(econtext), &ep_params, &econtext->client->server_ep);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "ucp_ep_create() failed");

    struct ucx_context *addr_size_request = NULL;
    struct ucx_context *addr_request = NULL;
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = ucx_client_boostrap_send_cb;
    send_param.datatype = ucp_dt_make_contig(1);
    send_param.user_data = &(econtext->client->bootstrapping.addr_size_ctx);

    /* 1. Send the address size */
    ucp_tag_t ucp_tag = MAKE_SEND_TAG(econtext->client->conn_data.oob.tag, 0, 0, 0, 0);
    econtext->client->bootstrapping.addr_size_request = ucp_tag_send_nbx(econtext->client->server_ep, &(econtext->client->conn_data.oob.local_addr_len), sizeof(size_t), ucp_tag, &send_param);
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
    client->conn_data.oob.peer_addr_len = addr_len;
    client->conn_data.oob.peer_addr = MALLOC(client->conn_data.oob.peer_addr_len);
    CHECK_ERR_GOTO((client->conn_data.oob.peer_addr == NULL), error_out, "Unable to allocate memory");
    size_recvd = recv(client->conn_data.oob.sock, client->conn_data.oob.peer_addr, client->conn_data.oob.peer_addr_len, MSG_WAITALL);
    DBG("Received the address (size: %ld)", size_recvd);
    size_recvd = recv(client->conn_data.oob.sock, &(client->server_id), sizeof(client->server_id), MSG_WAITALL);
    DBG("Received the server ID (size: %ld)", size_recvd);
    size_recvd = recv(client->conn_data.oob.sock, &(client->id), sizeof(client->id), MSG_WAITALL);
    DBG("Received the client ID (size: %ld)", size_recvd);
    econtext->client->bootstrapping.phase = OOB_CONNECT_DONE;
    ECONTEXT_UNLOCK(econtext);
    DBG("%s() done", __func__);
    return DO_SUCCESS;

error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

dpu_offload_status_t finalize_connection_to_remote_dpu(offloading_engine_t *offload_engine, remote_dpu_info_t *remote_dpu_info, execution_context_t *client)
{
    DBG("Connection successfully established");
    ENGINE_LOCK(offload_engine);
    remote_dpu_info_t **list_dpus = (remote_dpu_info_t **)offload_engine->dpus.base;
    list_dpus[remote_dpu_info->idx]->ep = client->client->server_ep;
    list_dpus[remote_dpu_info->idx]->econtext = client;
    list_dpus[remote_dpu_info->idx]->peer_addr = client->client->conn_data.oob.peer_addr;
    list_dpus[remote_dpu_info->idx]->ucp_worker = GET_WORKER(client);
    assert(list_dpus[remote_dpu_info->idx]->peer_addr);
    DBG("-> DPU #%ld: addr=%s, port=%d, ep=%p, econtext=%p",
        remote_dpu_info->idx,
        list_dpus[remote_dpu_info->idx]->init_params.conn_params->addr_str,
        list_dpus[remote_dpu_info->idx]->init_params.conn_params->port,
        list_dpus[remote_dpu_info->idx]->ep,
        list_dpus[remote_dpu_info->idx]->econtext);

    if (offload_engine->default_econtext == NULL)
        offload_engine->default_econtext = client;
    offload_engine->num_connected_dpus++;
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
    ENGINE_LOCK(engine);
    bool on_dpu = engine->on_dpu;
    ENGINE_UNLOCK(engine);
    if (on_dpu)
    {
        // Progress connections between DPUs when necessary
        size_t c, s, num_inter_dpus_clients;
        ENGINE_LOCK(engine);
        num_inter_dpus_clients = engine->num_inter_dpus_clients;
        ENGINE_UNLOCK(engine);
        for (c = 0; c < num_inter_dpus_clients; c++)
        {
            ENGINE_LOCK(engine);
            execution_context_t *c_econtext = engine->inter_dpus_clients[c].client_econtext;
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
                remote_dpu_info_t *remote_dpu_info = NULL;
                DBG("DPU client #%ld just finished its connection to a server, updating data", c);
                ECONTEXT_UNLOCK(c_econtext);
                dpu_offload_status_t rc = finalize_connection_to_remote_dpu(engine, engine->inter_dpus_clients[c].remote_dpu_info, engine->inter_dpus_clients[c].client_econtext);
                CHECK_ERR_RETURN((rc), DO_ERROR, "finalize_connection_to_remote_dpu() failed");
                ECONTEXT_UNLOCK(c_econtext);
            }
            ECONTEXT_UNLOCK(c_econtext);
        }
        progress_servers(engine);

        // Progress all the execution context in the engine
        size_t i;
        remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ENGINE(engine);
        for (i = 0; i < engine->num_dpus; i++)
        {
            if (list_dpus[i] == NULL)
                continue;

            execution_context_t *econtext = list_dpus[i]->econtext;
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

dpu_offload_status_t offload_engine_init(offloading_engine_t **engine)
{
    offloading_engine_t *d = MALLOC(sizeof(offloading_engine_t));
    CHECK_ERR_RETURN((d == NULL), DO_ERROR, "Unable to allocate resources");
    int ret = pthread_mutex_init(&(d->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));
    d->done = 0;
    d->ucp_worker = NULL;
    d->ucp_context = NULL;
    d->client = NULL;
    d->num_max_servers = DEFAULT_MAX_NUM_SERVERS;
    d->default_econtext = NULL;
    d->num_servers = 0;
    d->servers = MALLOC(d->num_max_servers * sizeof(dpu_offload_server_t));
    d->num_inter_dpus_clients = 0;
    d->num_max_inter_dpus_clients = DEFAULT_MAX_NUM_SERVERS;
    d->inter_dpus_clients = MALLOC(d->num_max_inter_dpus_clients * sizeof(remote_dpu_connect_tracker_t));
    CHECK_ERR_RETURN((d->servers == NULL), DO_ERROR, "unable to allocate resources");
    DYN_LIST_ALLOC(d->free_op_descs, 8, op_desc_t, item);
    DYN_LIST_ALLOC(d->free_peer_cache_entries, DEFAULT_NUM_PEERS, peer_cache_entry_t, item);
    CHECK_ERR_RETURN((d->free_peer_cache_entries == NULL), DO_ERROR, "Allocation of pool of free cache entries failed");
    DYN_LIST_ALLOC(d->free_peer_descs, DEFAULT_NUM_PEERS, peer_data_t, item);
    CHECK_ERR_RETURN((d->free_peer_descs == NULL), DO_ERROR, "Allocation of pool of proc descriptor failed");
    DYN_LIST_ALLOC(d->pool_conn_params, 32, conn_params_t, item);
    CHECK_ERR_RETURN((d->pool_conn_params == NULL), DO_ERROR, "Allocation of pool of connection parameter descriptors failed");
    // Note that engine->dpus is a vector of remote_dpu_info_t pointers.
    // The actual object are from a dynamic array when parsing the configuration file
    DYN_ARRAY_ALLOC(&(d->dpus), 32, remote_dpu_info_t *);
    GROUPS_CACHE_INIT(&(d->procs_cache));
    dpu_offload_status_t rc = ev_channels_init(&(d->default_notifications));
    CHECK_ERR_RETURN((rc), DO_ERROR, "ev_channels_init() failed");

    *engine = d;
    return DO_SUCCESS;
}

static void progress_event_recv(execution_context_t *econtext)
{
#if USE_AM_IMPLEM
    /* Nothing to do */
#else
    ECONTEXT_LOCK(econtext);
    // Progress the ongoing communications
    if (econtext->type == CONTEXT_SERVER)
    {
        // SERVER
        size_t n_client = 0;
        size_t idx = 0;
        // DBG("%ld connected clients, checking their status to receive notifications", econtext->server->connected_clients.num_connected_clients);
        while (n_client < econtext->server->connected_clients.num_connected_clients)
        {
            peer_info_t *client_info;
            DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), idx, peer_info_t, client_info);
            if (client_info == NULL || client_info->bootstrapping.phase != BOOTSTRAP_DONE)
            {
                DBG("Client #%ld not fully bootstrapped", idx);
                idx++;
                continue;
            }

            // DBG("Checking client #%ld", idx);
            ucp_worker_h worker = GET_WORKER(econtext);
            if (client_info->notif_recv.initialized == false)
            {
                // Note: tag and tag mask are calculated by the macro to match the client
                // i.e., only messages from that client will be received.
                DBG("Preparing reception of notification from client #%ld", idx);
                PREP_NOTIF_RECV(client_info->notif_recv.ctx,
                                client_info->notif_recv.hdr_recv_params,
                                client_info->notif_recv.hdr_ucp_tag,
                                client_info->notif_recv.hdr_ucp_tag_mask,
                                worker,
                                idx);
                client_info->notif_recv.initialized = true;
            }
            /*
            else
                DBG("Reception of notification already all set");
            */
            // The function will handle whether a new receive is required to be posted or not
            post_new_notif_recv(worker,
                                &(client_info->notif_recv.ctx),
                                econtext,
                                client_info->notif_recv.hdr_ucp_tag,
                                client_info->notif_recv.hdr_ucp_tag_mask,
                                &(client_info->notif_recv.hdr_recv_params));
            idx++;
            n_client++;
        }
    }
    else
    {
        // CLIENT
        ucp_worker_h worker = GET_WORKER(econtext);
        if (econtext->event_channels->notif_recv.initialized == false)
        {
            // Note: tag and tag mask are calculated by the macro to match the client
            // i.e., only messages from the sever will be received.
            PREP_NOTIF_RECV(econtext->event_channels->notif_recv.ctx,
                            econtext->event_channels->notif_recv.hdr_recv_params,
                            econtext->event_channels->notif_recv.hdr_ucp_tag,
                            econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                            worker,
                            econtext->client->id);
            econtext->event_channels->notif_recv.initialized = true;
        }

        // The function will handle whether a new receive is required to be posted or not
        post_new_notif_recv(worker,
                            &(econtext->event_channels->notif_recv.ctx),
                            econtext,
                            econtext->event_channels->notif_recv.hdr_ucp_tag,
                            econtext->event_channels->notif_recv.hdr_ucp_tag_mask,
                            &(econtext->event_channels->notif_recv.hdr_recv_params));
    }
    ECONTEXT_UNLOCK(econtext);
#endif
}

static void execution_context_progress(execution_context_t *ctx)
{
    assert(ctx);

    // Progress the UCX worker to eventually complete some communications
    ECONTEXT_LOCK(ctx);
    ucp_worker_h worker = GET_WORKER(ctx);
    ECONTEXT_UNLOCK(ctx);
    ucp_worker_progress(worker);

    if (ctx->type == CONTEXT_SERVER && ctx->server->connected_clients.num_ongoing_connections > 0)
    {
        size_t i = 0;
        size_t idx = 0;
        // Find the clients that are currently connecting
        while (i < ctx->server->connected_clients.num_ongoing_connections)
        {
            peer_info_t *client_info;
            DYN_ARRAY_GET_ELT(&(ctx->server->connected_clients.clients), idx, peer_info_t, client_info);
            if (client_info == NULL)
            {
                idx++;
                i++;
                continue;
            }

            // DBG("Progressing connection of client #%ld\n", i);
            if (client_info->bootstrapping.phase == OOB_CONNECT_DONE)
            {
                dpu_offload_status_t rc;
                if (client_info->bootstrapping.addr_size_ctx.complete == false && client_info->bootstrapping.addr_size_request == NULL)
                {
                    DBG("UCX level bootstrap - step 1 - Getting address size");
                    rc = oob_server_ucx_client_connection_step1(ctx, client_info);
                    CHECK_ERR_GOTO((rc), error_out, "oob_server_ucx_client_connection_step1() failed");
                }

                if (client_info->bootstrapping.addr_size_ctx.complete == true && client_info->bootstrapping.addr_size_request != NULL)
                {
                    // The send did not complete right away but now is, some clean up to do
                    ucp_request_free(client_info->bootstrapping.addr_size_request);
                    client_info->bootstrapping.addr_size_request = NULL;
                }

                if (client_info->bootstrapping.addr_size_ctx.complete == true && client_info->bootstrapping.addr_ctx.complete == false && client_info->bootstrapping.addr_request == NULL)
                {
                    DBG("UCX level bootstrap - step 2 - Getting address");
                    rc = oob_server_ucx_client_connection_step2(ctx, client_info);
                    CHECK_ERR_GOTO((rc), error_out, "oob_server_ucx_client_connection_step2() failed");
                }

                if (client_info->bootstrapping.addr_ctx.complete == true && client_info->bootstrapping.addr_request != NULL)
                {
                    // The send did not complete right away but now is, some clean up to do
                    ucp_request_free(client_info->bootstrapping.addr_request);
                    client_info->bootstrapping.addr_request = NULL;
                }

                if (client_info->bootstrapping.addr_ctx.complete == true && client_info->bootstrapping.rank_ctx.complete == false && client_info->bootstrapping.rank_request == NULL)
                {
                    DBG("UCX level bootstrap - step 3 - Getting rank info");
                    rc = oob_server_ucx_client_connection_step3(ctx, client_info);
                    CHECK_ERR_GOTO((rc), error_out, "oob_server_ucx_client_connection_step3() failed");
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
                    DBG("group/rank received: (%" PRId64 "/%" PRId64 ")", client_info->rank_data.group_id, client_info->rank_data.group_rank);
                    if (client_info->rank_data.group_id != INVALID_GROUP && client_info->rank_data.group_rank != INVALID_RANK)
                    {
                        // Update the pointer to track cache entries, i.e., groups/ranks, for the peer
                        DBG("Adding gp/rank %" PRId64 "/%" PRId64 " to cache", client_info->rank_data.group_id, client_info->rank_data.group_rank);
                        peer_cache_entry_t *cache_entry = SET_GROUP_RANK_CACHE_ENTRY(ctx, client_info->rank_data.group_id, client_info->rank_data.group_rank);
                        CHECK_ERR_GOTO((cache_entry == NULL), error_out, "undefined cache entry");
                        cache_entry->peer.addr_len = client_info->peer_addr_len;
                        if (client_info->peer_addr != NULL)
                        {
                            assert(client_info->peer_addr_len < MAX_ADDR_LEN);
                            memcpy(cache_entry->peer.addr,
                                   client_info->peer_addr,
                                   client_info->peer_addr_len);
                        }
                        assert(client_info->cache_entries.num_elts > 0);
                        peer_cache_entry_t **cache_entries = (peer_cache_entry_t **)(client_info->cache_entries.base);
                        assert(cache_entries);
                        cache_entries[0] = cache_entry;
                    }

                    if (ECONTEXT_ON_DPU(ctx))
                    {
                        /* Check if it is a DPU we are expecting to connect to us */
                        size_t dpu;
                        remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ECONTEXT(ctx);
                        CHECK_ERR_GOTO((list_dpus == NULL), error_out, "unable to get list of DPUs");
                        assert(ctx->engine);
                        for (dpu = 0; dpu < ctx->engine->num_dpus; dpu++)
                        {
                            if (SELF_DPU(ctx->engine, dpu))
                                continue;

                            CHECK_ERR_GOTO((list_dpus[dpu]->init_params.conn_params == NULL),
                                           error_out,
                                           "connection parameters of DPU #%ld is undefined", i);
                            CHECK_ERR_GOTO((list_dpus[dpu]->init_params.conn_params->addr_str == NULL),
                                           error_out,
                                           "Address of DPU #%ld is undefined", dpu);
                            if (strncmp(list_dpus[dpu]->init_params.conn_params->addr_str, client_info->peer_addr, client_info->peer_addr_len) == 0)
                            {
                                // Set the endpoint to communicate with that remote DPU
                                list_dpus[dpu]->ep = client_info->ep;
                                // Set the pointer to the execution context in the list of know DPUs. Used for notifications with the remote DPU.
                                list_dpus[dpu]->econtext = ctx;
                                DBG("-> DPU #%ld: addr=%s, port=%d, ep=%p", dpu,
                                    list_dpus[dpu]->init_params.conn_params->addr_str,
                                    list_dpus[dpu]->init_params.conn_params->port,
                                    list_dpus[dpu]->ep);
                                break;
                            }
                        }
                    }

                    client_info->bootstrapping.phase = BOOTSTRAP_DONE;
                    ctx->server->connected_clients.num_ongoing_connections--;
                    ctx->server->connected_clients.num_connected_clients++;
                    DBG("****** Bootstrapping of client #%ld now completed, %ld are now connected",
                        idx,
                        ctx->server->connected_clients.num_connected_clients);

                    if (ctx->server->connected_cb != NULL)
                    {
                        DBG("Invoking connection completion callback...");
                        connected_peer_data_t cb_data = {
                            .peer_addr = client_info->peer_addr,
                            .econtext = ctx,
                            .peer_id = i,
                        };
                        ctx->server->connected_cb(&cb_data);
                    }
                    ECONTEXT_UNLOCK(ctx);
                }
            }
            idx++;
            i++;
        }
    }
    else
    {
        // CLIENT
        if (ctx->client->bootstrapping.phase == OOB_CONNECT_DONE)
        {
            dpu_offload_status_t rc;
            // Need now to progress UCX bootstrapping
            if (ctx->client->bootstrapping.addr_size_ctx.complete == false && ctx->client->bootstrapping.addr_size_request == NULL)
            {
                DBG("UCX level bootstrap - step 1");
                rc = client_ucx_bootstrap_step1(ctx);
                CHECK_ERR_GOTO((rc), error_out, "client_ucx_bootstrap_step1() failed");
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
                CHECK_ERR_GOTO((rc), error_out, "client_ucx_boostrap_step2() failed");
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
                CHECK_ERR_GOTO((rc), error_out, "client_ucx_boostrap_step3() failed");
            }

            if (ctx->client->bootstrapping.rank_ctx.complete == true && ctx->client->bootstrapping.rank_request != NULL)
            {
                // The send did not complete right away but now is, some clean up to do
                ucp_request_free(ctx->client->bootstrapping.rank_request);
                ctx->client->bootstrapping.rank_request = NULL;
                ctx->client->bootstrapping.phase = UCX_CONNECT_DONE;
            }

            if (ctx->client->bootstrapping.addr_size_ctx.complete == true &&
                ctx->client->bootstrapping.addr_ctx.complete == true &&
                ctx->client->bootstrapping.rank_ctx.complete == true &&
                ctx->client->bootstrapping.addr_size_request == NULL &&
                ctx->client->bootstrapping.addr_request == NULL &&
                ctx->client->bootstrapping.rank_request == NULL)
            {
                ctx->client->bootstrapping.phase = BOOTSTRAP_DONE;
                if (ctx->client->connected_cb != NULL)
                {
                    DBG("Successfully connected, invoking connected callback");
                    connected_peer_data_t cb_data;
                    cb_data.peer_addr = ctx->client->conn_params.addr_str;
                    cb_data.econtext = ctx;
                    ctx->client->connected_cb(&cb_data);
                }
            }
        }
    }

    // Progress reception of events
    progress_event_recv(ctx);

    // Progress the ongoing events
    ECONTEXT_LOCK(ctx);
    dpu_offload_event_t *ev, *next_ev;
    ucs_list_for_each_safe(ev, next_ev, (&(ctx->ongoing_events)), item)
    {
        if (event_completed(ev))
        {
            ucs_list_del(&(ev->item));
            // DBG("event %p completed and removed from ongoing events list", ev);
            if (ev->req)
            {
                ucp_request_free(ev->req);
                ev->req = NULL;
            }
            event_return(&ev);
        }
    }

    // Progress pending emits
    while (!ucs_list_is_empty(&(ctx->event_channels->pending_emits)) &&
           ucs_list_length(&(ctx->event_channels->pending_emits)) < ctx->event_channels->max_pending_emits)
    {
        dpu_offload_event_t *event = ucs_list_extract_head(&(ctx->event_channels->pending_emits), dpu_offload_event_t, item);
#if USE_AM_IMPLEM
        int rc = am_send_event_msg(&event);
#else
        int rc = tag_send_event_msg(&event);
#endif // USE_AM_IMPLEM
        if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
            ucs_list_add_tail(&(ctx->event_channels->pending_emits), &(event->item));
    }

    // Progress all active operations
    dpu_offload_status_t rc = progress_active_ops(ctx);
error_out:
    ECONTEXT_UNLOCK(ctx);
    return;
}

static dpu_offload_status_t execution_context_init(offloading_engine_t *offload_engine, uint64_t type, execution_context_t **econtext)
{
    execution_context_t *ctx = MALLOC(sizeof(execution_context_t));
    CHECK_ERR_GOTO((ctx == NULL), error_out, "unable to allocate execution context");
    int ret = pthread_mutex_init(&(ctx->mutex), NULL);
    CHECK_ERR_GOTO((ret), error_out, "pthread_mutex_init() failed: %s", strerror(errno));
    ctx->type = type;
    ctx->engine = offload_engine;
    ctx->progress = execution_context_progress;
    ucs_list_head_init(&(ctx->ongoing_events));
    DYN_LIST_ALLOC(ctx->free_pending_rdv_recv, 32, pending_am_rdv_recv_t, item);
    ucs_list_head_init(&(ctx->pending_rdv_recvs));
    ucs_list_head_init(&(ctx->active_ops));
    *econtext = ctx;
    return DO_SUCCESS;
error_out:
    return DO_ERROR;
}

static void execution_context_fini(execution_context_t **ctx)
{
    size_t i;
    assert(ucs_list_is_empty(&((*ctx)->pending_rdv_recvs)));
    for (i = 0; i < (*ctx)->free_pending_rdv_recv->num_elts; i++)
    {
        pending_am_rdv_recv_t *elt;
        DYN_LIST_GET((*ctx)->free_pending_rdv_recv, pending_am_rdv_recv_t, item, elt);
        if (elt == NULL)
            continue;
        if (elt->buff_size > 0)
        {
            free(elt->user_data);
            elt->buff_size = 0;
        }
    }

    DYN_LIST_FREE((*ctx)->free_pending_rdv_recv, pending_am_rdv_recv_t, item);
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
    CHECK_ERR_GOTO((offload_engine == NULL), error_out, "Undefined handle");
    CHECK_ERR_GOTO((offload_engine->client != NULL), error_out, "offload engine already initialized as a client");

    execution_context_t *ctx;
    int rc = execution_context_init(offload_engine, CONTEXT_CLIENT, &ctx);
    CHECK_ERR_GOTO((rc != 0 || ctx == NULL), error_out, "execution_context_init() failed");
    if (init_params != NULL && init_params->proc_info != NULL)
    {
        ctx->rank.group_id = init_params->proc_info->group_id;
        ctx->rank.group_rank = init_params->proc_info->group_rank;
    }
    else
    {
        ctx->rank.group_id = INVALID_GROUP;
        ctx->rank.group_rank = INVALID_RANK;
    }
    DBG("execution context successfully initialized");

    ucs_status_t status;
    rc = client_init_context(ctx, init_params);
    CHECK_ERR_GOTO((rc), error_out, "init_client_context() failed");
    CHECK_ERR_GOTO((ctx->client == NULL), error_out, "client handle is undefined");
    DBG("client context successfully initialized");

    rc = event_channels_init(ctx);
    CHECK_ERR_GOTO((rc), error_out, "event_channel_init() failed");
    CHECK_ERR_GOTO((ctx->event_channels == NULL), error_out, "event channel object is undefined");
    ctx->client->event_channels = ctx->event_channels;
    ctx->client->event_channels->econtext = (struct execution_context *)ctx;
    DBG("event channels successfully initialized");

    /* Initialize Active Message data handler */
    dpu_offload_status_t ret = dpu_offload_set_am_recv_handlers(ctx);
    CHECK_ERR_GOTO((ret), error_out, "dpu_offload_set_am_recv_handlers() failed");
    DBG("Active messages successfully initialized");

    switch (ctx->client->mode)
    {
    case UCX_LISTENER:
    {
        ucx_listener_client_connect(ctx->client);
        break;
    }
    default:
    {
        ret = oob_connect(ctx);
        CHECK_ERR_GOTO((ret), error_out, "oob_connect() failed");
    }
    }

    ECONTEXT_LOCK(ctx);
    rc = register_default_notifications(ctx->event_channels);
    ECONTEXT_UNLOCK(ctx);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notfications() failed");

    DBG("%s() done", __func__);
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
    event_channels_fini(&((*offload_engine)->default_notifications));
    GROUPS_CACHE_FINI(&((*offload_engine)->procs_cache));
    DYN_LIST_FREE((*offload_engine)->free_op_descs, op_desc_t, item);
    DYN_LIST_FREE((*offload_engine)->free_peer_cache_entries, peer_cache_entry_t, item);
    DYN_ARRAY_FREE(&((*offload_engine)->dpus));
    free((*offload_engine)->client);
    int i;
    for (i = 0; i < (*offload_engine)->num_servers; i++)
    {
        // server_fini() fixme
    }
    for (i = 0; i < (*offload_engine)->num_inter_dpus_clients; i++)
    {
        client_fini(&((*offload_engine)->inter_dpus_clients[i].client_econtext));
    }
    free((*offload_engine)->servers);
    free(*offload_engine);
    *offload_engine = NULL;
}

void client_fini(execution_context_t **exec_ctx)
{
    if (exec_ctx == NULL || *exec_ctx == NULL)
        return;

    execution_context_t *context = *exec_ctx;
    if (context->type != CONTEXT_CLIENT)
    {
        ERR_MSG("invalid type");
        return;
    }

    DBG("Sending termination message to associated server...");
    void *term_msg;
    size_t msg_length = 0;
    ucp_request_param_t params;
    am_req_t ctx;
    ctx.complete = 0;
    params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_DATATYPE |
                          UCP_OP_ATTR_FIELD_USER_DATA;
    params.datatype = ucp_dt_make_contig(1);
    params.user_data = &ctx;
    params.cb.send = (ucp_send_nbx_callback_t)send_cb;
    dpu_offload_client_t *client = context->client;
    void *request = ucp_am_send_nbx(client->server_ep, AM_TERM_MSG_ID, NULL, 0ul, term_msg,
                                    msg_length, &params);
    // request_finalize(GET_WORKER(*exec_ctx), request, &ctx);
    DBG("Termination message successfully sent");

    ep_close(GET_WORKER(*exec_ctx), client->server_ep);
    dpu_offload_set_am_recv_handlers(context);

    switch (context->client->mode)
    {
    case UCX_LISTENER:
        break;
    default:
        // OOB
        if (client->conn_data.oob.sock > 0)
        {
            close(client->conn_data.oob.sock);
            client->conn_data.oob.sock = -1;
        }
        if (client->conn_data.oob.addr_msg_str != NULL)
        {
            free(client->conn_data.oob.addr_msg_str);
            client->conn_data.oob.addr_msg_str = NULL;
        }
        if (client->conn_data.oob.peer_addr != NULL)
        {
            free(client->conn_data.oob.peer_addr);
            client->conn_data.oob.peer_addr = NULL;
        }
        ucp_worker_release_address(GET_WORKER(*exec_ctx), client->conn_data.oob.local_addr);
    }

    event_channels_fini(&(client->event_channels));
    ucp_worker_destroy(GET_WORKER(*exec_ctx));

    free(context->client);
    context->client = NULL;

    execution_context_fini(&context);
}

static void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucx_server_ctx_t *context = arg;
    ucp_conn_request_attr_t attr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];
    ucs_status_t status;
    DBG("Connection handler invoked");
    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK)
    {
        DBG("Server received a connection request from client at address %s:%s", ip_str, port_str);
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
    struct sockaddr_storage listen_addr;
    ucp_listener_params_t params;
    ucp_listener_attr_t attr;
    ucs_status_t status;
    char *port_str;
    set_sock_addr(server->conn_params.addr_str, server->conn_params.port, &(server->conn_params.saddr));
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                        UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr = (const struct sockaddr *)&(server->conn_params.saddr);
    params.sockaddr.addrlen = sizeof(server->conn_params.saddr);
    params.conn_handler.cb = server_conn_handle_cb;
    params.conn_handler.arg = &(server->conn_data.ucx_listener.context);
    /* Create a listener on the server side to listen on the given address.*/
    DBG("Creating listener on %s:%s", server->conn_params.addr_str, port_str);
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
    DBG("server is listening on IP %s port %s", server->conn_params.addr_str, port_str);
    return DO_SUCCESS;
}

static void oob_recv_addr_handler(void *request, ucs_status_t status,
                                  ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *)request;
    context->completed = 1;

    DBG("receive handler for addr called with status %d (%s), length %lu",
        status, ucs_status_string(status), info->length);
}

static void oob_recv_rank_handler(void *request, ucs_status_t status,
                                  ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *)request;
    context->completed = 1;

    DBG("receive handler for rank called with status %d (%s), length %lu",
        status, ucs_status_string(status), info->length);
}

// This function assumes the execution context is properly locked before it is invoked
static inline uint64_t generate_unique_client_id(execution_context_t *econtext)
{
    // For now we only use the slot that the client will have in the list of connected clients
    return (uint64_t)(econtext->server->connected_clients.num_connected_clients + econtext->server->connected_clients.num_ongoing_connections);
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
    ssize_t size_sent = send(econtext->server->conn_data.oob.sock, &(econtext->server->conn_data.oob.local_addr_len), sizeof(econtext->server->conn_data.oob.local_addr_len), 0);
    DBG("Addr length send (len: %ld)", size_sent);
    size_sent = send(econtext->server->conn_data.oob.sock, econtext->server->conn_data.oob.local_addr, econtext->server->conn_data.oob.local_addr_len, 0);
    DBG("Address sent (len: %ld)", size_sent);
    size_sent = send(econtext->server->conn_data.oob.sock, &(econtext->server->id), sizeof(econtext->server->id), 0);
    DBG("Server ID sent (len: %ld)", size_sent);
    uint64_t client_id = generate_unique_client_id(econtext);
    size_sent = send(econtext->server->conn_data.oob.sock, &client_id, sizeof(uint64_t), 0);
    DBG("Client ID sent (len: %ld)", size_sent);

    /* All done, the rest is done when progressing the execution contexts */
    peer_info_t *client_info;
    DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), client_id, peer_info_t, client_info);
    assert(client_info);
    client_info->bootstrapping.phase = OOB_CONNECT_DONE;
    econtext->server->connected_clients.num_ongoing_connections++;
    ECONTEXT_UNLOCK(econtext);
    return DO_SUCCESS;
error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
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

            pthread_exit(NULL); // fixme: properly support multiple connections
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

#if 0
    // Wait for at least one client to connect
    DBG("Waiting for at least one client to connect...");
    bool client_connected = false;
    while (!client_connected)
    {
        econtext->progress(econtext);
        if (econtext->server->connected_clients.num_connected_clients > 0)
            client_connected++;
    }
    DBG("At least one client is now connected");
#endif

    return DO_SUCCESS;
}

#define MAX_CACHE_ENTRIES_PER_PROC (8)
dpu_offload_status_t server_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    int ret;
    size_t i;
    econtext->type = CONTEXT_SERVER;
    econtext->server = MALLOC(sizeof(dpu_offload_server_t));
    CHECK_ERR_RETURN((econtext->server == NULL), DO_ERROR, "Unable to allocate server handle");
    econtext->server->econtext = (struct execution_context *)econtext;
    econtext->server->mode = OOB; // By default, we connect with the OOB mode
    DYN_ARRAY_ALLOC(&(econtext->server->connected_clients.clients), DEFAULT_MAX_NUM_CLIENTS, peer_info_t);
    econtext->server->connected_clients.num_connected_clients = 0;
    econtext->server->connected_clients.num_ongoing_connections = 0;
    econtext->server->connected_cb = NULL;
    for (i = 0; i < DEFAULT_MAX_NUM_CLIENTS; i++)
    {
        peer_info_t *peer_info;
        DYN_ARRAY_GET_ELT(&(econtext->server->connected_clients.clients), i, peer_info_t, peer_info);
        peer_info->bootstrapping.phase = BOOTSTRAP_NOT_INITIATED;
        peer_info->bootstrapping.addr_ctx.complete = false;
        peer_info->bootstrapping.addr_size_ctx.complete = false;
        peer_info->bootstrapping.rank_ctx.complete = false;
        peer_info->notif_recv.ctx.complete = true; // to make we can post the initial recv
        assert(peer_info);
        DYN_ARRAY_ALLOC(&(peer_info->cache_entries), MAX_CACHE_ENTRIES_PER_PROC, peer_cache_entry_t *);
    }

    if (init_params == NULL || init_params->conn_params == NULL)
    {
        DBG("no initialization parameters specified, try to gather parameters from environment...");
        ret = get_env_config(&(econtext->server->conn_params));
        CHECK_ERR_RETURN((ret), DO_ERROR, "get_env_config() failed");
    }
    else
    {
        DBG("using connection parameters that have been passed in (init_params=%p, conn_params=%p, econtext=%p, econtext->conn_params=%p, addr=%s)",
            init_params, init_params->conn_params, econtext, &(econtext->server->conn_params), init_params->conn_params->addr_str);
        econtext->server->conn_params.addr_str = init_params->conn_params->addr_str;
        econtext->server->conn_params.port = init_params->conn_params->port;
        econtext->server->conn_params.port_str = NULL;
        if (init_params->id_set)
            econtext->server->id = init_params->id;
    }

    if (init_params != NULL && init_params->connected_cb != NULL)
    {
        econtext->server->connected_cb = init_params->connected_cb;
    }

    ret = pthread_mutex_init(&(econtext->server->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));

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
        DBG("server initialized with OOB backend");
        econtext->server->conn_data.oob.tag = OOB_DEFAULT_TAG;
        econtext->server->conn_data.oob.tag_mask = UINT64_MAX;
        econtext->server->conn_data.oob.addr_msg_str = strdup(UCX_ADDR_MSG);
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

static ucs_status_t server_create_ep(ucp_worker_h data_worker,
                                     ucp_conn_request_h conn_request,
                                     ucp_ep_h *server_ep)
{
    ucp_ep_params_t ep_params;
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

    DBG("initializing server context...");
    rc = server_init_context(execution_context, init_params);
    CHECK_ERR_GOTO((rc), error_out, "server_init_context() failed");
    CHECK_ERR_GOTO((execution_context->server == NULL), error_out, "undefined server handle");
    DBG("server handle %p successfully created (worker=%p)", execution_context->server, GET_WORKER(execution_context));
    offloading_engine->servers[offloading_engine->num_servers] = execution_context;
    offloading_engine->num_servers++;

    rc = event_channels_init(execution_context);
    CHECK_ERR_GOTO((rc), error_out, "event_channel_init() failed");
    CHECK_ERR_GOTO((execution_context->event_channels == NULL), error_out, "event channel handle is undefined");
    DBG("event channel %p successfully initialized", execution_context->event_channels);
    execution_context->server->event_channels = execution_context->event_channels;
    execution_context->server->event_channels->econtext = (struct execution_context *)execution_context;

    /* Initialize Active Message data handler */
    rc = dpu_offload_set_am_recv_handlers(execution_context);
    CHECK_ERR_GOTO((rc), error_out, "dpu_offload_set_am_recv_handlers() failed");

    /* Start the server to accept connections */
    ucs_status_t status = start_server(execution_context);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "start_server() failed");

#if !NDEBUG
    if (init_params != NULL && init_params->conn_params != NULL)
        DBG("Server created on %s:%d\n", init_params->conn_params->addr_str, init_params->conn_params->port);
#endif

    ECONTEXT_LOCK(execution_context);
    dpu_offload_ev_sys_t *ev_sys = execution_context->event_channels;
    rc = register_default_notifications(ev_sys);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notfications() failed");

    ENGINE_LOCK(offloading_engine);
    if (offloading_engine->num_default_notifications > 0)
    {
        notification_callback_entry_t *list_callbacks = (notification_callback_entry_t *)offloading_engine->default_notifications->notification_callbacks.base;
        size_t i;
        size_t n = 0;
        for (i = 0; i < offloading_engine->default_notifications->notification_callbacks.num_elts; i++)
        {
            if (list_callbacks[i].set)
            {
                rc = event_channel_register(execution_context->event_channels, i, list_callbacks[i].cb);
                CHECK_ERR_GOTO((rc), error_out, "unable to register engine's default notification to new execution context (type: %ld)", i);
                n++;
            }

            if (n == offloading_engine->num_default_notifications)
            {
                // All done, no need to continue parsing the array.
                break;
            }
        }
    }
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
    pthread_join(server->connect_tid, NULL);
    pthread_mutex_destroy(&(server->mutex));

    /* Close the clients' endpoint */
    for (i = 0; i < server->connected_clients.num_connected_clients; i++)
    {
        peer_info_t *peer_info;
        DYN_ARRAY_GET_ELT(&(server->connected_clients.clients), i, peer_info_t, peer_info);
        assert(peer_info);
        ep_close(GET_WORKER(*exec_ctx), peer_info->ep);
    }

    dpu_offload_set_am_recv_handlers(context);

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

    event_channels_fini(&(server->event_channels));
    ucp_worker_destroy(GET_WORKER(*exec_ctx));

    free(context->server);
    context->server = NULL;

    execution_context_fini(&context);
}
