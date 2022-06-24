# Overview and core capabilities

To enable the offloading of operations to the DPU, we provide a set of core 
capabilities:
- A *configuration file* that describes the entire platform (so not only the job) and provide basic data about the configuration of the infrastructure.
- *Service processes* that are running on DPUs and available for the offloading of operations. They provide a service for offloading on a specific DPU.
- *Execution contexts* to abstract the bootstrapping of the offloading service and it associated communications and notifications.
- *Notifications and events* to asynchronously interact with service processes running on DPUs.

Additional capabilities are under development:
- *Offloaded operations* to ease the implementation and execution of offloaded operation.
- *Remote service and binary start* to let applications start a new service binary on DPUs.

## Configuration file

Please refer to [configfile.md](./configfile.md)

## DPU service processes

Service processes, also called *daemons* are running on the DPUs to provide
a offloading service. New daemons can be implemented to tailor applications'
needs and separate daemons for the offloading of MPI collectives are under
development.

A service process, from an architectural point-of-view, is composed of two
different parts:
- A set of execution contexts for inter-service-processes interactions.
- An execution context for interactions with processes running on the host. It is currently assumed that a single execution context per service process is used, even if from a architecture point-of-view, there is no such limitation.

For inter-service-process interactions, the current implementation currently assumes full-connectivity betweeen all service processes and a separate execution context is available for each remote service process.

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

The execution context for interacting with the host is placed at the end of the vector containing all the execution context. In our example, the execution context for interacting with the host would therefore be in the 64^th^ slot.

Based on this scheme, it is possible to asynchronously create all these 
connections and also uniquely identify communications between two execution 
contexts, whereever their location.

Here is an example of a service process code that reads the configuration 
file and create all the execution contexts:
```
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