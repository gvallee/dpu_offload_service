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

extern dpu_offload_status_t register_default_notifications(dpu_offload_ev_sys_t *);

// A lot of the code is from ucp_client_serrver.c from UCX

static sa_family_t ai_family = AF_INET;

#define DEFAULT_MAX_NUM_CLIENTS (256)
#define DEFAULT_MAX_NUM_SERVERS (256)

#define IP_STRING_LEN 50
#define PORT_STRING_LEN 8
#define OOB_DEFAULT_TAG 0x1337a880u
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

static int oob_server_accept(execution_context_t *econtext, char *client_addr)
{
    int connfd;
    struct sockaddr_in servaddr;
    int optval = 1;
    int rc;

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
        CHECK_ERR_RETURN((rc), DO_ERROR, "bind() failed: %s", strerror(errno));
        rc = listen(econtext->server->conn_data.oob.listenfd, 1024);
        CHECK_ERR_RETURN((rc), DO_ERROR, "listen() failed: %s", strerror(errno));

        // fixme: debug when multi-connections are required
        DBG("Accepting connection on port %" PRIu16 "...", server_port);
        struct sockaddr_in addr;
        int addr_len = sizeof(client_addr);
        econtext->server->conn_data.oob.sock = accept(econtext->server->conn_data.oob.listenfd, (struct sockaddr *)&addr, &addr_len);
        struct in_addr ipAddr = addr.sin_addr;
        inet_ntop(AF_INET, &ipAddr, client_addr, INET_ADDRSTRLEN);
        DBG("Connection accepted from %s on fd=%d", client_addr, econtext->server->conn_data.oob.sock);
    }
    return DO_SUCCESS;
}

#define MAX_RETRY (300) // Never try to connect for more than 5 minutes

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

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    fprintf(stderr, "error handling callback was invoked with status %d (%s)\n",
            status, ucs_status_string(status));
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

static dpu_offload_status_t init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    int ret = DO_SUCCESS;
    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_worker_create() failed: %s", ucs_status_string(status));
    return ret;
}

static dpu_offload_status_t init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
{
    /* UCP objects */
    ucp_params_t ucp_params;
    ucs_status_t status;
    ucp_config_t *config;
    int ret = 0;
    memset(&ucp_params, 0, sizeof(ucp_params));
    /* UCP initialization */
    status = ucp_config_read(NULL, NULL, &config);
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_AM;
    status = ucp_init(&ucp_params, config, ucp_context);
    CHECK_ERR_GOTO((status != UCS_OK), err, "ucp_init() failed: %s", ucs_status_string(status));
    ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
    ucp_config_release(config);
    ret = init_worker(*ucp_context, ucp_worker);
    CHECK_ERR_GOTO((ret != 0), err_cleanup, "init_worker() failed");
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
    ctx = user_data;
    ctx->complete = 1;
}

static void send_cb(void *request, ucs_status_t status, void *user_data)
{
    common_cb(user_data, "send_cb");
}

dpu_offload_status_t client_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    int ret;
    dpu_offload_status_t rc;
    econtext->type = CONTEXT_CLIENT;
    econtext->client = malloc(sizeof(dpu_offload_client_t));
    CHECK_ERR_RETURN((econtext->client == NULL), DO_ERROR, "Unable to allocate client handle\n");
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

    // If we are not using a worker that was passed in, we create a new one.
    if (init_params == NULL || init_params->worker == NULL)
    {
        rc = init_context(&(econtext->client->ucp_context), &(econtext->client->ucp_worker));
        CHECK_ERR_RETURN((rc), DO_ERROR, "init_context() failed (rc: %d)", rc);
    }
    else
    {
        econtext->client->ucp_context = NULL;
        econtext->client->ucp_worker = init_params->worker;
    }

    // If we have a group/rank in the init params, we pass it down
    if (init_params != NULL)
    {
        econtext->rank.group_id = init_params->proc_info->group_id;
        econtext->rank.group_rank = init_params->proc_info->group_rank;
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

        ucs_status_t status = ucp_worker_get_address(econtext->client->ucp_worker,
                                                     &(econtext->client->conn_data.oob.local_addr),
                                                     &(econtext->client->conn_data.oob.local_addr_len));
        CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_worker_get_address() failed");
    }
    }

    return DO_SUCCESS;
}

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
    ucs_status_t status = ucp_ep_create(client->ucp_worker, &ep_params, &(client->server_ep));
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "failed to connect to %s (%s)", client->conn_params.addr_str,
                     ucs_status_string(status));
    DBG("Endpoint %p successfully created", client->server_ep);
    return DO_SUCCESS;
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, struct ucx_context *request,
                             const char *op_str, const char *data_str)
{
    ucs_status_t status;

    DBG("Waiting for request %p to complete", request);
    if (request == NULL)
    {
        // The request completed right away, nothing to do.
        return UCS_OK;
    }
    assert(ucp_worker);

    if (UCS_PTR_IS_ERR(request))
    {
        status = UCS_PTR_STATUS(request);
    }
    else if (UCS_PTR_IS_PTR(request))
    {
        while (!request->completed)
        {
            ucp_worker_progress(ucp_worker);
        }

        request->completed = 0;
        status = ucp_request_check_status(request);
        ucp_request_free(request);
    }
    else
    {
        status = UCS_OK;
    }

    if (status != UCS_OK)
    {
        ERR_MSG("unable to %s %s (%s)", op_str, data_str, ucs_status_string(status));
    }
    else
    {
        if (data_str != NULL)
            DBG("%s completed, data=%s", op_str, data_str);
        else
            DBG("%s completed", op_str);
    }

    return status;
}

static void oob_send_cb(void *request, ucs_status_t status, void *ctx)
{
    struct ucx_context *context = (struct ucx_context *)request;
    const char *str = (const char *)ctx;

    context->completed = 1;

    DBG("send handler called for \"%s\" with status %d (%s)", str, status, ucs_status_string(status));
}

static dpu_offload_status_t oob_connect(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined client handle");
    dpu_offload_client_t *client = econtext->client;
    CHECK_ERR_RETURN((client->conn_data.oob.local_addr == NULL), DO_ERROR, "undefined local address");
    DBG("local address length: %lu", client->conn_data.oob.local_addr_len);

    size_t addr_len;
    int rc = oob_client_connect(client, ai_family);
    CHECK_ERR_RETURN((rc), DO_ERROR, "oob_client_connect() failed");
    ssize_t size_recvd = recv(client->conn_data.oob.sock, &addr_len, sizeof(addr_len), MSG_WAITALL);
    DBG("Addr len received (len: %ld): %ld", size_recvd, addr_len);
    client->conn_data.oob.peer_addr_len = addr_len;
    client->conn_data.oob.peer_addr = malloc(client->conn_data.oob.peer_addr_len);
    CHECK_ERR_RETURN((client->conn_data.oob.peer_addr == NULL), DO_ERROR, "Unable to allocate memory");
    size_recvd = recv(client->conn_data.oob.sock, client->conn_data.oob.peer_addr, client->conn_data.oob.peer_addr_len, MSG_WAITALL);
    DBG("Received the address (size: %ld)", size_recvd);
    size_recvd = recv(client->conn_data.oob.sock, &(client->id), sizeof(client->id), MSG_WAITALL);
    DBG("Received the client ID (size: %ld)", size_recvd);

    /* Establish the UCX level connection */
    ucp_ep_params_t ep_params;
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address = client->conn_data.oob.peer_addr;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data = &(client->server_ep_status);

    ucs_status_t status = ucp_ep_create(client->ucp_worker, &ep_params, &client->server_ep);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "ucp_ep_create() failed");

    size_t msg_len = sizeof(uint64_t) + client->conn_data.oob.local_addr_len;
    DBG("Allocating msg (len: %ld)", msg_len);
    struct oob_msg *msg = malloc(msg_len);
    CHECK_ERR_RETURN((msg == NULL), DO_ERROR, "Memory allocation failed for msg");
    memset(msg, 0, msg_len);
    DBG("sending local addr to server, len=%ld", msg_len);
    msg->len = client->conn_data.oob.local_addr_len;
    memcpy(msg + 1, client->conn_data.oob.local_addr, client->conn_data.oob.local_addr_len);

    struct ucx_context *addr_request = NULL;
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = oob_send_cb;
    send_param.user_data = (void *)client->conn_data.oob.addr_msg_str;
    // We send everything required to create an endpoint to the server
    addr_request = ucp_tag_send_nbx(client->server_ep, msg, msg_len, client->conn_data.oob.tag,
                                    &send_param);
    // Because this is part of the bootstrapping, we wait for the sends to complete
    ucx_wait(client->ucp_worker, addr_request, "send",
             client->conn_data.oob.addr_msg_str);

    // We send our "rank", i.e., unique application ID
    DBG("Sending my group/rank info (%"PRId64"/%"PRId64"), len=%ld", econtext->rank.group_id, econtext->rank.group_rank, sizeof(econtext->rank));
    struct ucx_context *rank_request = NULL;
    send_param.user_data = NULL;
    rank_request = ucp_tag_send_nbx(client->server_ep, &(econtext->rank), sizeof(rank_info_t), client->conn_data.oob.tag + 1,
                                    &send_param);
    ucx_wait(client->ucp_worker, rank_request, "send", NULL);

    free(msg);
    msg = NULL;

    return DO_SUCCESS;
}

dpu_offload_status_t offload_engine_progress(offloading_engine_t *engine)
{
    if (engine->on_dpu)
    {
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
    return DO_SUCCESS;
}

dpu_offload_status_t lib_progress(execution_context_t *econtext)
{
    if (econtext->engine != NULL)
        return offload_engine_progress(econtext->engine);
    else
        return econtext->progress(econtext);
}

dpu_offload_status_t offload_engine_init(offloading_engine_t **engine)
{
    offloading_engine_t *d = malloc(sizeof(offloading_engine_t));
    CHECK_ERR_RETURN((d == NULL), DO_ERROR, "Unable to allocate resources");
    int ret = pthread_mutex_init(&(d->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));
    d->done = 0;
    d->client = NULL;
    d->num_max_servers = DEFAULT_MAX_NUM_SERVERS;
    d->default_econtext = NULL;
    d->num_servers = 0;
    d->servers = malloc(d->num_max_servers * sizeof(dpu_offload_server_t));
    d->num_inter_dpus_clients = 0;
    d->num_max_inter_dpus_clients = DEFAULT_MAX_NUM_SERVERS;
    d->inter_dpus_clients = malloc(d->num_max_inter_dpus_clients * sizeof(execution_context_t));
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

static dpu_offload_status_t execution_context_progress(execution_context_t *ctx)
{
    // Progress the UCX worker to eventually complete some communications
    ECONTEXT_LOCK(ctx);
    ucp_worker_progress(GET_WORKER(ctx));
    ECONTEXT_UNLOCK(ctx);

    // Progress the ongoing events
    dpu_offload_event_t *ev, *next_ev;
    ucs_list_for_each_safe(ev, next_ev, (&(ctx->ongoing_events)), item)
    {
        if (ev->ctx.complete)
        {
            ucs_list_del(&(ev->item));
            DBG("event %p completed and removed from ongoing events list", ev);
            if (ev->req)
            {
                ucp_request_free(ev->req);
                ev->req = NULL;
            }
            event_return(ctx->event_channels, &ev);
        }
    }

    // Progress all active operations
    return progress_active_ops(ctx);
}

static dpu_offload_status_t execution_context_init(offloading_engine_t *offload_engine, uint64_t type, execution_context_t **econtext)
{
    execution_context_t *ctx = malloc(sizeof(execution_context_t));
    CHECK_ERR_GOTO((ctx == NULL), error_out, "unable to allocate execution context");
    ctx->type = CONTEXT_CLIENT;
    ctx->engine = offload_engine;
    ctx->progress = execution_context_progress;
    ucs_list_head_init(&(ctx->ongoing_events));
    DYN_LIST_ALLOC(ctx->free_pending_rdv_recv, 32, pending_am_rdv_recv_t, item);
    ucs_list_head_init(&(ctx->pending_rdv_recvs));
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
    ctx->client->event_channels->econtext = (struct execution_context *)ctx;
    ctx->client->event_channels = ctx->event_channels;
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

    rc = register_default_notifications(ctx->event_channels);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notfications() failed");

    return ctx;
error_out:
    if (offload_engine->client != NULL)
    {
        free(offload_engine->client);
        offload_engine->client = NULL;
    }
    return NULL;
}

static ucs_status_t request_wait(ucp_worker_h ucp_worker, void *request, am_req_t *ctx)
{
    ucs_status_t status;

    /* if operation was completed immediately */
    if (request == NULL)
    {
        return UCS_OK;
    }

    if (UCS_PTR_IS_ERR(request))
    {
        return UCS_PTR_STATUS(request);
    }

    while (ctx->complete == 0)
    {
        ucp_worker_progress(ucp_worker);
    }
    status = ucp_request_check_status(request);

    ucp_request_free(request);

    return status;
}

static dpu_offload_status_t request_finalize(ucp_worker_h ucp_worker, void *request, am_req_t *ctx)
{
    ucs_status_t status = request_wait(ucp_worker, request, ctx);
    CHECK_ERR_RETURN((status != UCS_OK), DO_ERROR, "request failed: %s", ucs_status_string(status));
    return DO_SUCCESS;
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
        client_fini(&((*offload_engine)->inter_dpus_clients[i]));
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
    request_finalize(client->ucp_worker, request, &ctx);
    DBG("Termination message successfully sent");

    ep_close(client->ucp_worker, client->server_ep);
    ucp_worker_destroy(client->ucp_worker);

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
        ucp_worker_release_address(client->ucp_worker, client->conn_data.oob.local_addr);
    }

    event_channels_fini(&(client->event_channels));

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
    status = ucp_listener_create(server->ucp_worker, &params, &(server->conn_data.ucx_listener.context.listener));
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

static inline dpu_offload_status_t oob_server_ucx_client_connection(execution_context_t *econtext, char *client_addr)
{
    struct oob_msg *msg = NULL;
    struct ucx_context *addr_request = NULL;
    struct ucx_context *rank_request = NULL;
    size_t msg_len = 0;
    ucp_request_param_t send_param;
    ucp_tag_recv_info_t info_tag;
    ucs_status_t status;
    ucp_ep_params_t ep_params;
    ucp_tag_message_h msg_tag;
    int ret;

    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");

    dpu_offload_server_t *server = econtext->server;
    CHECK_ERR_RETURN((server == NULL), DO_ERROR, "server handle is undefined");

    DBG("Waiting for client's UCX info\n");

    /* Receive client UCX address */
    do
    {
        ECONTEXT_LOCK(econtext);
        /* Progressing before probe to update the state */
        ucp_worker_progress(server->ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(server->ucp_worker, server->conn_data.oob.tag, server->conn_data.oob.tag_mask, 1, &info_tag);
        ECONTEXT_UNLOCK(econtext);
    } while (msg_tag == NULL);

    ECONTEXT_LOCK(econtext);
    DBG("allocating space for message to receive: %ld", info_tag.length);
    msg = malloc(info_tag.length);
    CHECK_ERR_GOTO((msg == NULL), error_out, "unable to allocate memory");
    assert(server->ucp_worker);
    addr_request = ucp_tag_msg_recv_nb(server->ucp_worker, msg, info_tag.length,
                                       ucp_dt_make_contig(1), msg_tag, oob_recv_addr_handler);
    DBG("Receiving address length and data (len: %ld, req: %p)", info_tag.length, addr_request);
    status = ucx_wait(server->ucp_worker, addr_request, "receive", server->conn_data.oob.addr_msg_str);
    if (status != UCS_OK)
    {
        free(msg);
        return -1;
    }
    assert(addr_request);
    assert(server);
    assert(server->ucp_worker);
    assert(server->conn_data.oob.addr_msg_str);

    server->conn_data.oob.peer_addr_len = msg->len;
    CHECK_ERR_GOTO((msg <= 0), error_out, "invalid message length: %ld", msg->len);
    server->conn_data.oob.peer_addr = malloc(server->conn_data.oob.peer_addr_len);
    CHECK_ERR_GOTO((server->conn_data.oob.peer_addr == NULL), error_out, "unable to allocate memory for peer address (%ld bytes)", server->conn_data.oob.peer_addr_len);
    memcpy(server->conn_data.oob.peer_addr, msg + 1, server->conn_data.oob.peer_addr_len);
    free(msg);

    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
    ep_params.address = server->conn_data.oob.peer_addr;
    ep_params.user_data = &(server->connected_clients.clients[server->connected_clients.num_connected_clients].ep_status);
    ucp_worker_h worker = server->ucp_worker;
    CHECK_ERR_GOTO((worker == NULL), error_out, "undefined worker");
    ucp_ep_h client_ep;
    status = ucp_ep_create(worker, &ep_params, &client_ep);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "ucp_ep_create() failed: %s", ucs_status_string(status));
    CHECK_ERR_GOTO((server->connected_clients.num_connected_clients >= DEFAULT_MAX_NUM_CLIENTS),
                   error_out,
                   "max number of clients (%d) has been reached",
                   DEFAULT_MAX_NUM_CLIENTS);
    server->connected_clients.clients[server->connected_clients.num_connected_clients].ep = client_ep;
    DBG("endpoint successfully created");

    do
    {
        /* Progressing before probe to update the state */
        ucp_worker_progress(server->ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(server->ucp_worker, server->conn_data.oob.tag + 1, server->conn_data.oob.tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);
    DBG("Ready to receive peer data\n");
    rank_info_t peer_data;
    rank_request = ucp_tag_msg_recv_nb(server->ucp_worker, &peer_data, sizeof(rank_info_t),
                                       ucp_dt_make_contig(1), msg_tag, oob_recv_rank_handler);
    DBG("Receiving rank info (len=%ld, req=%p)", sizeof(rank_info_t), rank_request);
    ucx_wait(server->ucp_worker, rank_request, "receive", NULL);
    DBG("group/rank received: (%"PRId64"/%"PRId64")", peer_data.group_id, peer_data.group_rank);
    if (peer_data.group_id != INVALID_GROUP && peer_data.group_rank != INVALID_RANK)
    {
        // Update the pointer to track cache entries, i.e., groups/ranks, for the peer
        DBG("Adding gp/rank %"PRId64"/%"PRId64" to cache", peer_data.group_id, peer_data.group_rank);
        peer_cache_entry_t *cache_entry = SET_GROUP_RANK_CACHE_ENTRY(econtext, peer_data.group_id, peer_data.group_rank);
        assert(server->connected_clients.clients);
        assert(server->connected_clients.clients[server->connected_clients.num_connected_clients].cache_entries);
        server->connected_clients.clients[server->connected_clients.num_connected_clients].cache_entries[0] = cache_entry;
    }

    if (ECONTEXT_ON_DPU(econtext))
    {
        /* Check if it is a DPU we are expecting to connect to us */
        size_t i;
        remote_dpu_info_t **list_dpus = LIST_DPUS_FROM_ECONTEXT(econtext);
        CHECK_ERR_RETURN((list_dpus == NULL), DO_ERROR, "unable to get list of DPUs");
        for (i = 0; i < econtext->engine->num_dpus; i++)
        {
            if (list_dpus[i] == NULL)
                continue;

            CHECK_ERR_GOTO((list_dpus[i]->init_params.conn_params == NULL),
                           error_out,
                           "connection parameters of DPU #%ld is undefined", i);
            CHECK_ERR_GOTO((list_dpus[i]->init_params.conn_params->addr_str == NULL),
                           error_out,
                           "Address of DPU #%ld is undefined", i);
            assert(server);
            assert(server->conn_data.oob.peer_addr);
            if (strncmp(list_dpus[i]->init_params.conn_params->addr_str, client_addr, strlen(client_addr)) == 0)
            {
                // Set the endpoint to communicate with that remote DPU
                list_dpus[i]->ep = client_ep;
                // Set the pointer to the execution context in the list of know DPUs. Used for notifications with the remote DPU.
                list_dpus[i]->econtext = econtext;
                DBG("-> DPU #%ld: addr=%s, port=%d, ep=%p", i,
                    list_dpus[i]->init_params.conn_params->addr_str,
                    list_dpus[i]->init_params.conn_params->port,
                    list_dpus[i]->ep);
                break;
            }
        }
    }

    server->connected_clients.num_connected_clients++;
    ECONTEXT_UNLOCK(econtext);
    DBG("Endpoint to client successfully created");
    return DO_SUCCESS;

error_out:
    ECONTEXT_UNLOCK(econtext);
    return DO_ERROR;
}

static inline uint64_t generate_unique_client_id(execution_context_t *econtext)
{
    // For now we only use the slot that the client will have in the list of connected clients
    return (uint64_t)econtext->server->connected_clients.num_connected_clients;
}

static dpu_offload_status_t oob_server_listen(execution_context_t *econtext)
{
    /* OOB connection establishment */
    char client_addr[INET_ADDRSTRLEN];
    dpu_offload_status_t rc = oob_server_accept(econtext, client_addr);
    CHECK_ERR_RETURN((rc), DO_ERROR, "oob_server_accept() failed");
    DBG("Sending my worker's data...\n");
    ssize_t size_sent = send(econtext->server->conn_data.oob.sock, &(econtext->server->conn_data.oob.local_addr_len), sizeof(econtext->server->conn_data.oob.local_addr_len), 0);
    DBG("Addr length send (len: %ld)", size_sent);
    size_sent = send(econtext->server->conn_data.oob.sock, econtext->server->conn_data.oob.local_addr, econtext->server->conn_data.oob.local_addr_len, 0);
    DBG("Address sent (len: %ld)", size_sent);
    uint64_t client_id = generate_unique_client_id(econtext);
    size_sent = send(econtext->server->conn_data.oob.sock, &client_id, sizeof(uint64_t), 0);
    DBG("Client ID sent (len: %ld)", size_sent);
    
    rc = oob_server_ucx_client_connection(econtext, client_addr);
    CHECK_ERR_RETURN((rc), DO_ERROR, "oob_server_ucx_client_connection() failed");

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

    while (!econtext->server->done)
    {
        switch (econtext->server->mode)
        {
        case UCX_LISTENER:
        {
            ucx_listener_server(econtext->server);
            DBG("Waiting for connection on UCX listener...");
            while (econtext->server->conn_data.ucx_listener.context.conn_request == NULL)
            {
                DBG("Progressing worker...");
                ucp_worker_progress(econtext->server->ucp_worker);
            }
            break;
        }
        default:
        {
            int rc = oob_server_listen(econtext);
            if (rc)
            {
                ERR_MSG("oob_server_listen() failed");
                pthread_exit(NULL);
            }
            pthread_exit(NULL); // fixme: properly support multiple connections
        }
        }
    }

    pthread_exit(NULL);
}

static dpu_offload_status_t start_server(execution_context_t *econtext)
{
    CHECK_ERR_RETURN((econtext == NULL), DO_ERROR, "undefined execution context");
    CHECK_ERR_RETURN((econtext->server == NULL), DO_ERROR, "undefined server handle");

    int rc = pthread_create(&econtext->server->connect_tid, NULL, &connect_thread, econtext);
    CHECK_ERR_RETURN((rc), DO_ERROR, "unable to start connection thread");

    // Wait for at least one client to connect
    DBG("Waiting for at least one client to connect...");
    bool client_connected = false;
    while (!client_connected)
    {
        ECONTEXT_LOCK(econtext);
        if (econtext->server->connected_clients.num_connected_clients > 0)
            client_connected++;
        ECONTEXT_UNLOCK(econtext);
    }
    DBG("At least one client is now connected");

    return DO_SUCCESS;
}

#define MAX_CACHE_ENTRIES_PER_PROC (8)
dpu_offload_status_t server_init_context(execution_context_t *econtext, init_params_t *init_params)
{
    int ret;
    econtext->type = CONTEXT_SERVER;
    econtext->server = malloc(sizeof(dpu_offload_server_t));
    CHECK_ERR_RETURN((econtext->server == NULL), DO_ERROR, "Unable to allocate server handle");
    econtext->server->mode = OOB; // By default, we connect with the OOB mode
    econtext->server->connected_clients.clients = malloc(DEFAULT_MAX_NUM_CLIENTS * sizeof(peer_info_t));
    econtext->server->connected_clients.num_connected_clients = 0;
    CHECK_ERR_RETURN((econtext->server->connected_clients.clients == NULL), DO_ERROR, "Unable to allocate resources to track connected clients");
    int i;
    for (i = 0; i < DEFAULT_MAX_NUM_CLIENTS; i++)
    {
        econtext->server->connected_clients.clients[i].cache_entries = malloc(MAX_CACHE_ENTRIES_PER_PROC * sizeof(peer_cache_entry_t *));
        CHECK_ERR_RETURN((econtext->server->connected_clients.clients[i].cache_entries == NULL), DO_ERROR, "Unable to allocate resource to track ranks' cache entries");
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

    DBG("starting server connection thread");
    ret = pthread_mutex_init(&(econtext->server->mutex), NULL);
    CHECK_ERR_RETURN((ret), DO_ERROR, "pthread_mutex_init() failed: %s", strerror(errno));
    CHECK_ERR_RETURN((econtext->server->connected_clients.clients == NULL), DO_ERROR, "Unable to allocate resources for list of connected clients");

    if (init_params == NULL || init_params->worker == NULL)
    {
        ret = init_context(&(econtext->server->ucp_context), &(econtext->server->ucp_worker));
        CHECK_ERR_RETURN((ret != 0), DO_ERROR, "init_context() failed");
        DBG("context successfully initialized (worker=%p)", econtext->server->ucp_worker);
    }
    else
    {
        DBG("re-using UCP worker for new execution context");
        econtext->server->ucp_context = NULL;
        econtext->server->ucp_worker = init_params->worker;
    }

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
        ucs_status_t status = ucp_worker_get_address(econtext->server->ucp_worker,
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
    DBG("server handle successfully created (worker=%p)", execution_context->server->ucp_worker);
    offloading_engine->servers[offloading_engine->num_servers] = execution_context;
    offloading_engine->num_servers++;

    rc = event_channels_init(execution_context);
    CHECK_ERR_GOTO((rc), error_out, "event_channel_init() failed");
    CHECK_ERR_GOTO((execution_context->event_channels == NULL), error_out, "event channel handle is undefined");
    execution_context->server->event_channels->econtext = (struct execution_context *)execution_context;
    execution_context->server->event_channels = execution_context->event_channels;

    /* Initialize Active Message data handler */
    rc = dpu_offload_set_am_recv_handlers(execution_context);
    CHECK_ERR_GOTO((rc), error_out, "dpu_offload_set_am_recv_handlers() failed");

    ucs_status_t status = start_server(execution_context);
    CHECK_ERR_GOTO((status != UCS_OK), error_out, "start_server() failed");

#if !NDEBUG
    if (init_params != NULL && init_params->conn_params != NULL)
        DBG("Server created on %s:%d\n", init_params->conn_params->addr_str, init_params->conn_params->port);
#endif

    rc = register_default_notifications(execution_context->event_channels);
    CHECK_ERR_GOTO((rc), error_out, "register_default_notfications() failed");

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

    return execution_context;

error_out:
    if (execution_context != NULL)
    {
        free(execution_context);
    }
    return NULL;
}

void server_fini(execution_context_t **exec_ctx)
{
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
    int i;
    for (i = 0; i < server->connected_clients.num_connected_clients; i++)
    {
        ep_close(server->ucp_worker, server->connected_clients.clients[i].ep);
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
        ucp_worker_release_address(server->ucp_worker, server->conn_data.oob.local_addr);
        if (server->conn_data.oob.peer_addr != NULL)
        {
            free(server->conn_data.oob.peer_addr);
            server->conn_data.oob.peer_addr = NULL;
        }
    }
    ucp_worker_destroy(server->ucp_worker);

    event_channels_fini(&(server->event_channels));

    free(context->server);
    context->server = NULL;

    execution_context_fini(&context);
}
