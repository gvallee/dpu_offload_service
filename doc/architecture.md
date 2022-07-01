# Overview and core capabilities

To enable the offloading of operations to the DPU, we provide a set of core 
capabilities:
- A [configuration file](#configuration-file) that describes the entire platform (so not only the job) and provide basic data about the configuration of the infrastructure.
- A [offloading engine](#offloading-engine) that is the instantiation of a implementation of the offloading library. It is used both on the hosts and DPUs and are the core handle to store all the information related to a given offloading service. In other words, an engine is specific to a single offloading service.
- [Service processes](#dpu-service-processes) that are running on DPUs and available for the offloading of operations. They provide a service for offloading on a specific DPU.
- [Execution contexts](#execution-context) to abstract the bootstrapping of the offloading service and it associated communications and notifications.
- [Notifications and events](#notifications-and-events) to asynchronously interact with service processes running on DPUs.
- A [endpoint cache](#endpoint-cache) to know which service processes are associated to any rank in a group.

Additional capabilities are under development:
- *Offloaded operations* to ease the implementation and execution of offloaded operation.
- *Remote service and binary start* to let applications start a new service binary on DPUs.
- *Native support for XGVMI memory accesses*.

## Offloading engine

An offloading engine (type: `offloading_engine_t`) is the object that instantiates a given offloading 
service on both the host and the service processes running on the DPU. A service running on the DPU 
therefore has a single engine and an application using the offloading infrastructure has as many 
engine as the number of offloading services that are being used.

Please see the doxygen documentation for details about the engine API.

## Configuration file

Please refer to [configfile.md](./configfile.md).

## DPU service processes

Service processes (type: `remote_service_proc_info_t`), also called *daemons* are running on the DPUs
to provide a offloading service. New daemons can be implemented to tailor applications'
needs and separate daemons for the offloading of MPI collectives are under
development.

A service process, from an architectural point-of-view, is composed of two
different parts:
- A set of execution contexts for inter-service-processes interactions.
- An execution context for interactions with processes running on the host. It is currently assumed that a single execution context per service process is used, even if from a architecture point-of-view, there is no such limitation.

For inter-service-process interactions, the current implementation currently assumes 
full-connectivity betweeen all service processes and a separate execution context is available for 
each remote service process.

All service processes have a unique identifier. The current implementation 
relies on the configuration file to determine this unique identifier, which 
means that it is assumed that *the same configuration file is used on all 
DPUs and hosts*. Based on this unique identifier, a unique execution context
is created for each tuple of service processes, would be on the same DPU or
a remote DPU. These execution contexts are saved in a vector of execution 
contexts at the engine level. As an example, if the platform has 8 DPUs, 8 
services processes per DPU and the job is using all DPUs, the engine has a 
vector of service processes from 0 to 8 * 8 service processes, i.e., 0 to 64.
For all service processes, the global identifier is also used to identify the
*self execution context*, i.e., the execution context used to refer to itself
(and enable the implementation of optimization, e.g., notifications to self).

The execution context for interacting with the host is placed at the end of the vector containing all 
the execution context. In our example, the execution context for interacting with the host would 
therefore be in the 64^th^ slot.

Based on this scheme, it is possible to asynchronously create all these 
connections and also uniquely identify communications between two execution 
contexts, wherever their location.

## Execution context

An execution context (type: `execution_context_t`) is an object that abstract the details for 
interactions between two processes.
In other words, the object is available once the bootstrapping on the service between two entities,
e.g., host-service-process or inter-service-processes, and abstract the technical details of the
bootstrapping process.

Once an execution context is created, a set of capabilities are available:
- a connection state with the remote entity,
- notifications, including self-notifications,
- exposure of the underlying communication handles such as a UCX endpoint,
- a cache of communication handles so it is possible to lookup which service process is assigned to any application process, that can for example be later on used to initiate XGVMI operations.

## Notifications and events

Notifications, also called events, aims at facilitating the implementation of the control path of a
given algorithm (type: `dpu_offload_event_t`). In other words, it is only a capability to send 
notifications between execution contexts and while the associated interfaces offer similarities with 
an active message communication layer, it is not meant to be one mainly because missing advanced 
memory management capabilities. 
Active messages are available by using UCX and the endpoints available through the endpoint cache
and the execution contexts.

Notifications are composed of two entities: a header and a payload. The header is based on unique
identifier to uniquely identify the source, the destination and the type of the notification. The
payload can be any buffer; developers can request a buffer allocated by the infrastructure library
or manage their own memory and pass it in the notification system for performance.

The notification type is also used to register before hand a notification handler. Upon reception the
registered handler for the type is invoked, both the header and payload being provided within the
handler. At the moment, only one handler per notification type can be registered but developers are
free to implement handler that will invoke sub-handlers.

It is possible to emit events from within a notification handler.

It is also possible to create a hierarchy of events, i.e., to create sub-event within a given event.
In such a situation, the event containing sub-events must be a local event, i.e., should not be used
to send a notification to a remote execution context and is identified as a *meta-event*. A 
meta-event completes only when all the sub-events are completed.

The notification system (type: `dpu_offload_ev_sys_t`) is the core object used to implement the 
notification system. It provides a pool of free event that are available for use (using the `event_get
()` function). Note that it is possible to request specific features when getting an event, such as
requesting from the library an allocated buffer, by using a *dpu_offload_event_info_t* object.
Please refer to the doxygen documentation for details.

Once an event object is obtained, it is possible to set its payload and emit it. Note that two 
functions are available to emit an event:

- `event_channel_emit()`: which emits an event for which the payload is already specified.
- `event_channel_emit_with_payload()`: which emits an event and specify the payload at emission time.

When the event is emitting, it is by default added to a list of ongoing events. It is possible to
manually manage all events, which will prevent the event to be added to the ongoing list. Please
refer to the doxygen documentation for details.
When the event is on the ongoing list and completes, the event is implicitly returned to the event
system. If the event is manually handled, developers must return it by using the `event_return()`
function.

Please see the doxygen documentation for details about the datastructures and functions related to
the notification system.

## Endpoint cache

Please refer to [./endpoint_cache.md](./endpoint_cache.md) for details.

## Example

### Service bootstrapping between a service process on a DPU and the host process

Here is an example of a service process code that reads the configuration 
file and create all the execution contexts:
```
int main(int argc, char **argv)
{
    // Initialize the offloading engine
    offloading_engine_t *offload_engine;
    offload_engine_init(&offload_engine);

    // Load the configuration from the configuration file
    offloading_config_t config_data;
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    get_dpu_config(offload_engine, &config_data);

    // Initiate the connections between all the service processes.
    // This is an asynchronous & non-blocking operation.
    inter_dpus_connect_mgr(offload_engine, &config_data);

    // For illustration, wait for the local service process to be fully connected
    while (offload_engine->num_service_procs != offload_engine->num_connected_service_procs + 1)
    {
        offload_engine_progress(offload_engine);
    }

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */

    // We let the system figure out the configuration to use to let ranks connect
    SET_DEFAULT_DPU_HOST_SERVER_CALLBACKS(&(config_data.local_service_proc.host_init_params));
    // The first slots in the array of servers are reserved for inter-DPUs servers
    // where the index is equal to the DPU ID.
    config_data.local_service_proc.host_init_params.id = offload_engine->num_service_procs;
    config_data.local_service_proc.host_init_params.id_set = true;
    execution_context_t *service_server;
    service_server = server_init(offload_engine,
                                 &(config_data.local_service_proc.host_init_params));
```

The corresponding code for the host:

```
// Handle to store the configuration for offloading
offloading_config_t offload_config;
// Handle for the offloading engine
offloading_engine_t *engine;
// String storing the path to the configuration file to use
char *offloading_config_file;
// execution context to interact with the service process running on the DPU
execution_context_t *econtext;
// Initialization parameters that will be used for the creation of the 
// execution context.
init_params_t offloading_init_params;
// Connection parameters that will be used for bootstrapping of the 
// offloading service.
conn_params_t client_conn_params;
// Handle to store data about the current process, used during the
// boostrapping of the service.
rank_info_t rank_info;

// For MPI-like applications, a set of data about the group of processes 
// used by the application, the total number of processes running on the 
// hosts, as well as data about the processes running on the local host.
// This data is used to determine to which service process we are assigned.
int64_t my_rank, my_group, my_group_size, local_rank;
uint64_t n_local_ranks;

// Based on the application, developers are in charge of providing these
// functions/macros. For MPI application, a group equate to a communicator.
// It is also possible to not provide any of these elements but developers
// are then responsible for the development of a custom daemon and 
// potentially expend the current implementation.
my_group = GET_MY_GROUP();
my_group_size = GET_MY_GROUP_SIZE();
my_rank = GET_MY_RANK(my_group);
local_rank = GET_MY_LOCAL_RANK();
n_local_ranks = GET_N_LOCAL_RANKS();

rank_info.group_id = my_group;
rank_info.group_rank = my_rank;
rank_info.group_size = my_group_size;
rank_info.n_local_ranks = n_local_ranks;
rank_info.local_rank = local_rank;

// Get the configuration file path from the environment variable
offloading_config_file = getenv(OFFLOAD_CONFIG_FILE_PATH_ENVVAR);

// Initialize the offloading engine handle.
offload_engine_init(&engine);

// Assign it to the configuration handle.
dpus_config.cfg_file = offloading_config_file;
// Initialize the configuration handle.
INIT_DPU_CONFIG_DATA(&offload_config);
// Load the configuration from the configuration file
get_host_config(&offload_config);
// Assign the engine to the configuration
offload_config.offloading_engine = engine;

// Initialize the parameters that will be used to create the execution
// context for interactions with the service process running on the DPU.
RESET_INIT_PARAMS(&offloading_init_params);
RESET_CONN_PARAMS(&client_conn_params);
offloading_init_params.conn_params = &client_conn_params;
offloading_init_params.proc_info   = &rank_info;
// A new UCX worker and UCX context will be created. It is also possible to
// reuse an existing one.
offloading_init_params.worker      = NULL;
offloading_init_params.ucp_context = NULL;

// Figure out which service process running on the DPU is assigned to us
rc = get_local_service_proc_connect_info(&offload_config,
                                         &rank_info,
                                         &offloading_init_params);

// Boostrap the service, including the connection to the assigned service 
// process
econtext = client_init(engine, &offloading_init_params);

// Assign the execution context to the engine for implicit progress
engine->client = econtext;
```