# Overview and core capabilities

To enable the offloading of operations to the DPU, we provide a set of core 
capabilities:
- A [configuration file](#configuration-file) that describes the entire platform (so not only the job) and provide basic data about the configuration of the infrastructure.
- A [offloading engine](#offloading-engine) that is the instantiation of a implementation of the offloading library. It is used both on the hosts and DPUs and are the core handle to store all the information related to a given offloading service. In other words, an engine is specific to a single offloading service.
- [Service processes](#dpu-service-processes) that are running on DPUs and available for the offloading of operations. They provide a service for offloading on a specific DPU.
- [Execution contexts](#execution-context) to abstract the bootstrapping of the offloading service and it associated communications and notifications.
- [Notifications and events](#notifications-and-events) to asynchronously interact with service processes running on DPUs.
- A [endpoint cache](#endpoint-cache) to know which service processes are associated to any rank in a group.
- A set of APIs to [progress](#progress) communication, notifications and offloaded operations.

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

Details about notifications and events are available in [notifications](notifications.md).

## Endpoint cache

Please refer to [./endpoint_cache.md](./endpoint_cache.md) for details.

## Progress

The library provides the following progress capabilities:
- [lib_progress()](#progress-of-the-entire-library), which progresses the entire library and the main function used by developers since it includes all the following progress capabilities,
- [offload_engine_progress()](#progress-of-a-specific-offloading-engine), which progress a specific engine,
- and a [progress function for individual execution contexts](#progress-of-individual-contexts).

### Progress of the entire library

It is possible to progress the entire offloading library that has been previously instantiated.
It progresses all the offloading engine (see next section for details) and as a result, all existing
execution contexts, notifications and internal operations.

The library does not maintain any global object so it is necessary to pass in the current execution
context. From the execution context, the library finds all the objects instantiated within the
library that are needed to ensure progress of all communications, notifications and internal
operations.
```
lib_progress(my_econtext);
```

### Progress of a specific offloading engine

It is possible to progress an entire offloading engine, i.e., all its execution contexts and
all the notifications and operations associated to it.

Here is an example to progress the entire library:
```
offloading_engine_t *offload_engine;
offload_engine_init(&offload_engine);
...
offload_engine_progress(offload_engine);
```

Another example in the context of the MPI integration is to wait for all the data about the
current group/team/communicator to be gathered, which requires to progress the offloading
engine until the cache related to the team is fully populated:
```
do
{
    offload_engine_progress(offloading_engine);
} while (!group_cache_populated(offloading_engine, team_id))
```

### Progress of individual contexts

The progress function for execution context has the following signature:
```
void (*execution_context_progress_fn)(struct execution_context *);
```
A default progress function is assigned to the execution context.
As a result, progress is available as soon as the execution context has been successfully initialized.
Here is an example of the code to progress an execution context that is acting as a server:
```
offloading_engine_t *offload_engine;
execution_context_t *server;

offload_engine_init(&offload_engine);
// Create an execution context acting as a server
server = server_init(offload_engine, NULL);
// Progress the execution context
server->progress(server);
```

Because bootstrapping is asynchronous, the progress function can be used to now
when the bootstrapping of the execution context is completed, for example, for 
a client execution context:
```
offloading_engine_t *offload_engine;
execution_context_t *client;

offload_engine_init(&offload_engine);
// Create the client execution context using the environment variables
// to figure out how to connect to the associated server.
client = client_init(offload_engine, NULL);

// Wait until the execution context is fully connected to the server, i.e.,
// until the bootstrapping completes.
do
{
    client->progress(client);
} while (client->client->bootstrapping.phase != BOOTSTRAP_DONE);
```

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