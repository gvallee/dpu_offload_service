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

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_comm_channels.h"
#include "dpu_offload_event_channels.h"

// A lot of the code is from ucp_client_serrver.c from UCX

static sa_family_t ai_family = AF_INET;

#define SERVER_PORT_ENVVAR "DPU_OFFLOAD_SERVER_PORT"
#define SERVER_IP_ADDR_ENVVAR "DPU_OFFLOAD_SERVER_ADDR"

#define IP_STRING_LEN 50
#define PORT_STRING_LEN 8
#define OOB_DEFAULT_TAG 0x1337a880u
#define UCX_ADDR_MSG "UCX address message"

typedef struct am_msg_t
{
    volatile int complete;
    int is_rndv;
    void *desc;
    void *recv_buf;
} am_msg_t;

struct oob_msg
{
    uint64_t len;
};

am_msg_t am_data_desc = {0, 0, NULL, NULL};

int get_env_config(char **addr_str, char **port_str, uint16_t *port)
{
    char *server_port_envvar = getenv(SERVER_PORT_ENVVAR);
    char *server_addr = getenv(SERVER_IP_ADDR_ENVVAR);
    *port = -1;

    if (server_port_envvar)
    {
        *port = (uint16_t)atoi(server_port_envvar);
    }

    if (!server_addr)
    {
        fprintf(stderr, "Invalid server address, please make sure the environment variable %s is correctly set\n", SERVER_IP_ADDR_ENVVAR);
        return -1;
    }

    if (*port < 0)
    {
        fprintf(stderr, "Invalid server port (%s), please specify the environment variable %s\n", server_port_envvar, SERVER_PORT_ENVVAR);
        return -1;
    }

    *addr_str = server_addr;
    *port_str = server_port_envvar;

    return 0;
}

int set_sock_addr(char *addr, uint16_t port, struct sockaddr_storage *saddr)
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

    return 0;
}

static int oob_server_accept(uint16_t server_port, sa_family_t af)
{
    int listenfd, connfd;
    struct sockaddr_in servaddr;
    int optval = 1;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(listenfd, 1024);

    fprintf(stderr, "Accepting connection on port %" PRIu16 "...\n", server_port);
    connfd = accept(listenfd, (struct sockaddr *)NULL, NULL);
    fprintf(stderr, "Connection acception on fd=%d\n", connfd);

    return connfd;
}

int oob_client_connect(dpu_offload_client_t *client, sa_family_t af)
{
    int listenfd = -1;
    int optval = 1;
    char service[8];
    struct addrinfo hints, *res, *t;
    int ret;
    int rc = 0;

    snprintf(service, sizeof(service), "%u", client->port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = (client->address_str == NULL) ? AI_PASSIVE : 0;
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;

    fprintf(stderr, "Connecting to %s:%" PRIu16 "\n", client->address_str, client->port);

    ret = getaddrinfo(client->address_str, service, &hints, &res);
    if (ret < 0)
    {
        fprintf(stderr, "getaddrinfo() failed\n");
        return -1;
    }

    bool connected = false;
    for (t = res; t != NULL; t = t->ai_next)
    {
        client->conn_data.oob.sock = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (client->conn_data.oob.sock < 0)
        {
            continue;
        }

        fprintf(stderr, "Connecting to server...\n");
        int rc = connect(client->conn_data.oob.sock, t->ai_addr, t->ai_addrlen);
        if (rc == 0)
        {
            fprintf(stderr, "Connection established, fd = %d\n", client->conn_data.oob.sock);
            break;
        }
        else
        {
            fprintf(stderr, "Connection failed (rc: %d)\n", rc);
        }
    }

    if (client->conn_data.oob.sock < 0)
    {
        fprintf(stderr, "Unable to connect to server: invalid file descriptor (%d)\n", client->conn_data.oob.sock);
        return -1;
    }

out_free_res:
    freeaddrinfo(res);
out:
    return rc;
err_close_sockfd:
    close(client->conn_data.oob.sock);
    rc = -1;
    goto out_free_res;
}

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    printf("error handling callback was invoked with status %d (%s)\n",
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
        fprintf(stderr, "failed to close ep %p\n", (void *)ep);
    }
}

static int init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    int ret = 0;
    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker);
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to ucp_worker_create (%s)\n", ucs_status_string(status));
        ret = -1;
    }

    return ret;
}

static int init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
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
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to ucp_init (%s)\n", ucs_status_string(status));
        ret = -1;
        goto err;
    }
    ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
    ucp_config_release(config);
    ret = init_worker(*ucp_context, ucp_worker);
    if (ret != 0)
    {
        goto err_cleanup;
    }
    return ret;
err_cleanup:
    ucp_cleanup(*ucp_context);
err:
    return ret;
}

ucs_status_t ucp_am_data_cb(void *arg, const void *header, size_t header_length,
                            void *data, size_t length,
                            const ucp_am_recv_param_t *param)
{
    ucp_dt_iov_t *iov;
    size_t idx;
    size_t offset;
    static long iov_cnt = 1;
    static long test_string_length = 16;

    if (length != iov_cnt * test_string_length)
    {
        fprintf(stderr, "received wrong data length %ld (expected %ld)",
                length, iov_cnt * test_string_length);
        return UCS_OK;
    }
    if (header_length != 0)
    {
        fprintf(stderr, "received unexpected header, length %ld", header_length);
    }
    am_data_desc.complete = 1;
    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV)
    {
        /* Rendezvous request arrived, data contains an internal UCX descriptor,
         * which has to be passed to ucp_am_recv_data_nbx function to confirm
         * data transfer.
         */
        am_data_desc.is_rndv = 1;
        am_data_desc.desc = data;
        return UCS_INPROGRESS;
    }
    /* Message delivered with eager protocol, data should be available
     * immediately
     */
    am_data_desc.is_rndv = 0;
    iov = am_data_desc.recv_buf;
    offset = 0;
    for (idx = 0; idx < iov_cnt; idx++)
    {
        memcpy(iov[idx].buffer, UCS_PTR_BYTE_OFFSET(data, offset),
               iov[idx].length);
        offset += iov[idx].length;
    }
    return UCS_OK;
}

static void common_cb(void *user_data, const char *type_str)
{
    am_req_t *ctx;
    if (user_data == NULL)
    {
        fprintf(stderr, "user_data passed to %s mustn't be NULL\n", type_str);
        return;
    }
    ctx = user_data;
    ctx->complete = 1;
}

static void send_cb(void *request, ucs_status_t status, void *user_data)
{
    common_cb(user_data, "send_cb");
}

int client_init_context(dpu_offload_client_t **c)
{
    dpu_offload_client_t *client = malloc(sizeof(dpu_offload_client_t));
    if (client == NULL)
    {
        fprintf(stderr, "Unable to allocate client handle\n");
        return -1;
    }

    get_env_config(&client->address_str, &client->port_str, &(client->port));
    client->port = (uint16_t)atoi(client->port_str);

    int rc = init_context(&(client->ucp_context), &(client->ucp_worker));
    if (rc)
    {
        fprintf(stderr, "init_context() failed (rc: %d)\n", rc);
        return rc;
    }

    switch (client->mode)
    {
    case UCX_LISTENER:
        break;
    default:
    {
        // OOB
        client->conn_data.oob.addr_msg_str = strdup(UCX_ADDR_MSG);
        client->conn_data.oob.tag = OOB_DEFAULT_TAG;
        client->conn_data.oob.local_addr = NULL;
        client->conn_data.oob.peer_addr = NULL;
        client->conn_data.oob.local_addr_len = 0;
        client->conn_data.oob.peer_addr_len = 0;
        client->conn_data.oob.sock = -1;

        ucs_status_t status = ucp_worker_get_address(client->ucp_worker, &(client->conn_data.oob.local_addr), &(client->conn_data.oob.local_addr_len));
        if (status != UCS_OK)
        {
            fprintf(stderr, "ucp_worker_get_address() failed\n");
            return -1;
        }
    }
    }

    *c = client;
    return 0;
}

static int ucx_listener_client_connect(dpu_offload_client_t *client)
{
    set_sock_addr(client->address_str, client->port, &(client->conn_data.ucx_listener.connect_addr));

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
    fprintf(stderr, "Connecting to %s:%s\n", client->address_str, client->port_str);
    ucs_status_t status = ucp_ep_create(client->ucp_worker, &ep_params, &(client->server_ep));
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to connect to %s (%s)\n", client->address_str,
                ucs_status_string(status));
        return -1;
    }
    fprintf(stderr, "Endpoint %p successfully created\n", client->server_ep);

    return 0;
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, struct ucx_context *request,
                             const char *op_str, const char *data_str)
{
    ucs_status_t status;

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
        fprintf(stderr, "unable to %s %s (%s)\n", op_str, data_str,
                ucs_status_string(status));
    }
    else
    {
        fprintf(stderr, "%s of msg %s completed\n", op_str, data_str);
    }

    return status;
}

static void oob_send_cb(void *request, ucs_status_t status, void *ctx)
{
    struct ucx_context *context = (struct ucx_context *)request;
    const char *str = (const char *)ctx;

    context->completed = 1;

    printf("send handler called for \"%s\" with status %d (%s)\n",
           str, status, ucs_status_string(status));
}

static int oob_connect(dpu_offload_client_t *client)
{
    if (client->conn_data.oob.local_addr == NULL)
    {
        fprintf(stderr, "undefined local address\n");
        return -1;
    }
    printf("local address length: %lu\n", client->conn_data.oob.local_addr_len);

    size_t addr_len;
    int rc = oob_client_connect(client, ai_family);
    if (rc)
    {
        fprintf(stderr, "oob_client_connect() failed\n");
        return -1;
    }
    recv(client->conn_data.oob.sock, &addr_len, sizeof(addr_len), MSG_WAITALL);
    fprintf(stderr, "Addr len received: %ld\n", addr_len);
    client->conn_data.oob.peer_addr_len = addr_len;
    client->conn_data.oob.peer_addr = malloc(client->conn_data.oob.peer_addr_len);
    if (client->conn_data.oob.peer_addr == NULL)
    {
        fprintf(stderr, "Unable to allocate memory\n");
        return -1;
    }
    recv(client->conn_data.oob.sock, client->conn_data.oob.peer_addr, client->conn_data.oob.peer_addr_len, MSG_WAITALL);

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
    if (status != UCS_OK)
    {
        fprintf(stderr, "l.%d ucp_ep_create() failed\n", __LINE__);
        return -1;
    }

    size_t msg_len = sizeof(uint64_t) + client->conn_data.oob.local_addr_len;
    fprintf(stderr, "Allocating msg (len: %ld)\n", msg_len);
    struct oob_msg *msg = malloc(msg_len);
    if (msg == NULL)
    {
        fprintf(stderr, "Memory allocation failed for msg\n");
        return -1;
    }
    memset(msg, 0, msg_len);
    fprintf(stderr, "sending local addr to server, len=%ld\n", client->conn_data.oob.local_addr_len);
    msg->len = client->conn_data.oob.local_addr_len;
    memcpy(msg + 1, client->conn_data.oob.local_addr, client->conn_data.oob.local_addr_len);

    struct ucx_context *request;
    ucp_request_param_t send_param;
    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send = oob_send_cb;
    send_param.user_data = (void *)client->conn_data.oob.addr_msg_str;
    request = ucp_tag_send_nbx(client->server_ep, msg, msg_len, client->conn_data.oob.tag,
                               &send_param);
    status = ucx_wait(client->ucp_worker, request, "send",
                      client->conn_data.oob.addr_msg_str);

    return 0;
}

int client_init(dpu_offload_daemon_t **client)
{
    if (client == NULL)
    {
        fprintf(stderr, "Undefined handle\n");
        goto error_out;
    }

    dpu_offload_daemon_t *d = malloc(sizeof(dpu_offload_daemon_t));
    if (d == NULL)
    {
        fprintf(stderr, "Unable to allocate resources\n");
        goto error_out;
    }
    d->type = DAEMON_CLIENT;
    d->done = 0;

    ucs_status_t status;
    dpu_offload_client_t *_client;
    int rc = client_init_context(&_client);
    if (rc)
    {
        fprintf(stderr, "init_client_context() failed\n");
        goto error_out;
    }
    if (_client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        goto error_out;
    }
    d->client = _client;

    rc = event_channels_init(d);
    if (rc)
    {
        fprintf(stderr, "event_channel_init() failed\n");
        goto error_out;
    }

    /* Initialize Active Message data handler */
    int ret = dpu_offload_set_am_recv_handlers(d);
    if (ret)
    {
        fprintf(stderr, "dpu_offload_set_am_recv_handlers() failed\n");
        goto error_out;
    }

    switch (d->client->mode)
    {
    case UCX_LISTENER:
    {
        ucx_listener_client_connect(d->client);
        break;
    }
    default:
    {
        oob_connect(d->client);
    }
    }

    // The client is ready and wait for a message from the server for the initial handshake
    *client = d;

    return 0;
error_out:
    *client = NULL;
    return -1;
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

static int request_finalize(ucp_worker_h ucp_worker, void *request, am_req_t *ctx)
{
    int ret = 0;
    ucs_status_t status;

    status = request_wait(ucp_worker, request, ctx);
    if (status != UCS_OK)
    {
        fprintf(stderr, "request failed: %s\n", ucs_status_string(status));
        return -1;
    }

    return 0;
}

void client_fini(dpu_offload_daemon_t **c)
{
    if (c == NULL || *c == NULL)
        return;

    if ((*c)->client != NULL)
    {
        dpu_offload_client_t *client = (*c)->client;

        fprintf(stderr, "Sending termination message...\n");
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
        void *request = ucp_am_send_nbx(client->server_ep, AM_TERM_MSG_ID, NULL, 0ul, term_msg,
                                        msg_length, &params);
        request_finalize(client->ucp_worker, request, &ctx);

        ep_close(client->ucp_worker, client->server_ep);
        ucp_worker_destroy(client->ucp_worker);

        switch (client->mode)
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
        free((*c)->client);
        (*c)->client = NULL;
    }

    event_channels_fini(&((*c)->event_channels));

    free(*c);
    *c = NULL;
}

static void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucx_server_ctx_t *context = arg;
    ucp_conn_request_attr_t attr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];
    ucs_status_t status;
    fprintf(stderr, "Connection handler invoked\n");
    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK)
    {
        printf("Server received a connection request from client at address %s:%s\n",
               ip_str, port_str);
    }
    else if (status != UCS_ERR_UNSUPPORTED)
    {
        fprintf(stderr, "failed to query the connection request (%s)\n",
                ucs_status_string(status));
    }
    if (context->conn_request == NULL)
    {
        context->conn_request = conn_request;
    }
    else
    {
        /* The server is already handling a connection request from a client,
         * reject this new one */
        printf("Rejecting a connection request. "
               "Only one client at a time is supported.\n");
        status = ucp_listener_reject(context->listener, conn_request);
        if (status != UCS_OK)
        {
            fprintf(stderr, "server failed to reject a connection request: (%s)\n",
                    ucs_status_string(status));
        }
    }
}

static int ucx_listener_server(dpu_offload_server_t *server)
{
    struct sockaddr_storage listen_addr;
    ucp_listener_params_t params;
    ucp_listener_attr_t attr;
    ucs_status_t status;
    char *port_str;
    set_sock_addr(server->ip_str, server->port, &(server->saddr));
    params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                        UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr = (const struct sockaddr *)&(server->saddr);
    params.sockaddr.addrlen = sizeof(server->saddr);
    params.conn_handler.cb = server_conn_handle_cb;
    params.conn_handler.arg = &(server->conn_data.ucx_listener.context);
    /* Create a listener on the server side to listen on the given address.*/
    fprintf(stderr, "Creating listener on %s:%s\n", server->ip_str, port_str);
    status = ucp_listener_create(server->ucp_worker, &params, &(server->conn_data.ucx_listener.context.listener));
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to listen (%s)\n", ucs_status_string(status));
        return -1;
    }
    /* Query the created listener to get the port it is listening on. */
    attr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
    status = ucp_listener_query(server->conn_data.ucx_listener.context.listener, &attr);
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to query the listener (%s)\n",
                ucs_status_string(status));
        ucp_listener_destroy(server->conn_data.ucx_listener.context.listener);
        return -1;
    }
    fprintf(stderr, "server is listening on IP %s port %s\n",
            server->ip_str, port_str);
    return 0;
}

static void oob_recv_handler(void *request, ucs_status_t status,
                             ucp_tag_recv_info_t *info)
{
    struct ucx_context *context = (struct ucx_context *)request;

    context->completed = 1;

    printf("receive handler called with status %d (%s), length %lu\n",
           status, ucs_status_string(status), info->length);
}

static int oob_server_ucx_client_connection(dpu_offload_server_t *server)
{
    struct oob_msg *msg = NULL;
    struct ucx_context *request = NULL;
    size_t msg_len = 0;
    ucp_request_param_t send_param;
    ucp_tag_recv_info_t info_tag;
    ucs_status_t status;
    ucp_ep_h client_ep;
    ucp_ep_params_t ep_params;
    ucp_tag_message_h msg_tag;
    int ret;

    /* Receive client UCX address */
    do
    {
        /* Progressing before probe to update the state */
        ucp_worker_progress(server->ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(server->ucp_worker, server->conn_data.oob.tag, server->conn_data.oob.tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    fprintf(stderr, "allocating space for message to receive: %ld\n", info_tag.length);
    msg = malloc(info_tag.length);
    if (msg == NULL)
    {
        fprintf(stderr, "unable to allocate memory\n");
        return -1;
    }
    request = ucp_tag_msg_recv_nb(server->ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag, oob_recv_handler);
    status = ucx_wait(server->ucp_worker, request, "receive", server->conn_data.oob.addr_msg_str);
    if (status != UCS_OK)
    {
        free(msg);
        return -1;
    }

    server->conn_data.oob.peer_addr_len = msg->len;
    fprintf(stderr, "addr len for remote peer is %ld\n", server->conn_data.oob.peer_addr_len);
    server->conn_data.oob.peer_addr = malloc(server->conn_data.oob.peer_addr_len);
    if (server->conn_data.oob.peer_addr == NULL)
    {
        fprintf(stderr, "unable to allocate memory for peer address\n");
        return -1;
    }

    memcpy(server->conn_data.oob.peer_addr, msg + 1, server->conn_data.oob.peer_addr_len);
    free(msg);

#if 1
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                           UCP_EP_PARAM_FIELD_ERR_HANDLER |
                           UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.err_mode = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb = err_cb;
    ep_params.err_handler.arg = NULL;
#else
    ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                           UCP_EP_PARAM_FIELD_USER_DATA;
#endif
    ep_params.address = server->conn_data.oob.peer_addr;
    ep_params.user_data = &server->client_ep_status;
    status = ucp_ep_create(server->ucp_worker, &ep_params, &server->client_ep);
    if (status != UCS_OK)
    {
        fprintf(stderr, "ucp_ep_create() failed: %s\n", ucs_status_string(status));
        return -1;
    }
    fprintf(stderr, "Endpoint to client successfully created\n");

    return 0;
}

static int oob_server_listen(dpu_offload_server_t *server)
{
    /* OOB connection establishment */
    server->conn_data.oob.sock = oob_server_accept(server->port, ai_family);

    fprintf(stderr, "Sending add len on sock %d: %ld\n", server->conn_data.oob.sock, server->conn_data.oob.local_addr_len);
    send(server->conn_data.oob.sock, &(server->conn_data.oob.local_addr_len), sizeof(server->conn_data.oob.local_addr_len), 0);
    send(server->conn_data.oob.sock, server->conn_data.oob.local_addr, server->conn_data.oob.local_addr_len, 0);

    int rc = oob_server_ucx_client_connection(server);
    if (rc)
    {
        fprintf(stderr, "oob_server_ucx_client_connection() failed\n");
        return rc;
    }

    return 0;
}

static int start_server(dpu_offload_server_t *server)
{
    switch (server->mode)
    {
    case UCX_LISTENER:
    {
        ucx_listener_server(server);
        fprintf(stderr, "Waiting for connection on UCX listener...\n");
        while (server->conn_data.ucx_listener.context.conn_request == NULL)
        {
            fprintf(stderr, "Progressing worker...\n");
            ucp_worker_progress(server->ucp_worker);
        }
        break;
    }
    default:
    {
        int rc = oob_server_listen(server);
        if (rc)
        {
            fprintf(stderr, "oob_server_listen() failed\n");
            return -1;
        }
    }
    }

    return 0;
}

int server_init_context(dpu_offload_server_t **s)
{
    dpu_offload_server_t *server = malloc(sizeof(dpu_offload_server_t));
    if (server == NULL)
    {
        fprintf(stderr, "Unable to allocate server handle\n");
        return -1;
    }

    get_env_config(&server->ip_str, &server->port_str, &(server->port));
    server->port = (uint16_t)atoi(server->port_str);
    server->mode = OOB; // By default, we connect with the OOB mode

    int ret = init_context(&(server->ucp_context), &(server->ucp_worker));
    if (ret != 0)
    {
        fprintf(stderr, "init_context() failed\n");
        return -1;
    }

    switch (server->mode)
    {
    case UCX_LISTENER:
    {
        server->conn_data.ucx_listener.context.conn_request = NULL;
        break;
    }
    default:
    {
        // OOB
        server->conn_data.oob.tag = OOB_DEFAULT_TAG;
        server->conn_data.oob.tag_mask = UINT64_MAX;
        server->conn_data.oob.addr_msg_str = strdup(UCX_ADDR_MSG);
        server->conn_data.oob.peer_addr = NULL;
        server->conn_data.oob.local_addr = NULL;
        server->conn_data.oob.local_addr_len = 0;
        server->conn_data.oob.peer_addr_len = 0;
        ucs_status_t status = ucp_worker_get_address(server->ucp_worker, &(server->conn_data.oob.local_addr), &(server->conn_data.oob.local_addr_len));
        if (status != UCS_OK)
        {
            fprintf(stderr, "ucp_worker_get_address() failed\n");
            return -1;
        }
    }
    }

    *s = server;
    return 0;
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
    if (status != UCS_OK)
    {
        fprintf(stderr, "failed to create an endpoint on the server: (%s)\n",
                ucs_status_string(status));
    }
    return status;
}

int server_init(dpu_offload_daemon_t **s)
{
    if (s == NULL)
    {
        fprintf(stderr, "Handle is NULL\n");
        goto error_out;
    }

    dpu_offload_daemon_t *d = malloc(sizeof(dpu_offload_daemon_t));
    if (d == NULL)
    {
        fprintf(stderr, "Unable to allocate resources\n");
        goto error_out;
    }
    d->type = DAEMON_SERVER;
    d->done = 0;
    
    int ret = server_init_context(&(d->server));
    if (ret)
    {
        fprintf(stderr, "server_init_context() failed\n");
        goto error_out;
    }
    fprintf(stderr, "Server handle successfully created: %p\n", *s);

    ret = event_channels_init(d);
    if (ret)
    {
        fprintf(stderr, "event_channel_init() failed\n");
        goto error_out;
    }


    /* Initialize Active Message data handler */
    ret = dpu_offload_set_am_recv_handlers(d);
    if (ret)
    {
        fprintf(stderr, "dpu_offload_set_am_recv_handlers() failed\n");
        goto error_out;
    }

    ucs_status_t status = start_server(d->server);
    if (status != UCS_OK)
    {
        fprintf(stderr, "start_server() failed\n");
        goto error_out;
    }
    *s = d;

    fprintf(stderr, "Connection accepted\n");
    return 0;

error_out:
    *s = NULL;
    return -1;
}

void server_fini(dpu_offload_daemon_t **s)
{
    if (s == NULL || *s == NULL)
        return;
    dpu_offload_server_t *server = (*s)->server;

    if (server != NULL)
    {
        /* Close the endpoint to the client */
        ep_close(server->ucp_worker, server->client_ep);
        switch ((*s)->server->mode)
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
        free((*s)->server);
        (*s)->server = NULL;
    }

    event_channels_fini(&((*s)->event_channels));

    free(*s);
    *s = NULL;
}
