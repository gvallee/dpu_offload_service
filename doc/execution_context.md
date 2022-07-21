# Execution contexts

An execution context (type: `execution_context_t`) is an object that abstracts the details for 
interactions between two processes.
In other words, the object abstracts all details related to the bootstrapping of the service between two entities,
e.g., host-service-process or inter-service-processes.

Once an execution context is created, a set of capabilities are available:
- a connection state with the remote entity, also named bootstrapping state,
- notifications, including self-notifications,
- exposure of the underlying communication handles such as a UCX endpoint,
- a cache of communication handles so it is possible to lookup which service process is assigned to any application process in group, that can for example be later on used to initiate XGVMI operations.

## Initialization

An execution context is obtained by either starting a client or a server.
The concept of client and server are applicable in the context of bootstrapping.
A typical example is a service process running on a DPU: it starts a server, i.e.,
an execution context acting as a server, that accepts connections from the ranks
running on the local host. The ranks therefore have an execution context acting
as a client: during bootstrapping, it connects to a service process.

Users only need to pay attention to the fact that an execution context is a server
or a client when designing their service, mainly focusing on the initialization
phase based on which processes are created first. In our example, it is assumed
the service on the DPU is started before the ranks, it is therefore natural to
have execution contexts acting as servers on the DPU and client on the host to
handle the connection server-client.

Once the bootstrapping phase completed, users can most of time use generic APIs
that are applicable to both servers and clients.

The bootstrapping phase is available at any time through a bootstrap structure
that is available for both clients and servers. From a data structure point-of-view
it means the code is based on the following hierarchy:
```
execution_context_t
|__ // During bootstrapping, the execution context acts either as a client or server.
    union
    {
        dpu_offload_client_t *client;
        dpu_offload_server_t *server;
    };
```
and:
```
typedef struct dpu_offload_client_t
{
    bootstrapping_t bootstrapping;
    ...
} dpu_offload_client_t;
```
```
typedef struct dpu_offload_server_t
{
    bootstrapping_t bootstrapping;
    ...
} dpu_offload_server_t;
```
The `boostrapping_t` structure being:
```
typedef struct boostrapping
{
    int phase;
    struct ucx_context *addr_size_request;
    am_req_t addr_size_ctx;
    struct ucx_context *addr_request;
    am_req_t addr_ctx;
    struct ucx_context *rank_request;
    am_req_t rank_ctx;
} bootstrapping_t;
```

A helper macro is available to get the bootstrapping phase of an execution context without having to know if it is a client or a server:
```
GET_ECONTEXT_BOOTSTRAPING_PHASE(my_execution_context);
```

The following phases are defined:
- BOOTSTRAP_NOT_INITIATED, when no bootstrapping has been initialized, usually meaning the execution context object was just created.
- OOB_CONNECT_DONE: the bootstrapping phase related to out-of-band connection has been completed; only a socket between the client and server is available, no high-performance networking system.
- UCX_CONNECT_DONE: the bootstrapping of UCX between the client and the server has been completed; high-performance communication is now available between the two entities.
- BOOTSTRAP_DONE: all capabilities are fully initialized between the client and server, including but not limited to the high-performance networking and notifications.
- DISCONNECTED: all connections are now disconnected, a new bootstrapping is necessary to communicate again with the remote entity but no guarantee is provided that the state is suitable to initiate a new bootstrapping (especially in the context of an error).
- UNKNOWN: the state is unknown.

# Finalization

Similar to the initialization of a new execution context, the finalization is specific to the type of the execution context, i.e., client or server.

To finalize a client, please use `void client_fini(execution_context_t **exec_ctx)`. Upon completion, the execution context is set to NULL and therefore cannot be used any longer.

To finalize a server, please use `void server_fini(execution_context_t **exec_ctx)`. Upon completion, the execution context is set to NULL and therefore cannot be used any longer.

# Self execution context

The infrastructure provides a special execution context for every offloading engine: the self execution context.
This special execution context is meant to provide a pool of events that are not linked to a server or client context.
It is for example a suitable solution to handle notifications that are strictly local since the self execution context
provides a fully function notification sub-system.

The use of the self execution context is optional and not expected to be widely used.