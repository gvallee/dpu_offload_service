# Endpoint cache

## Introduction

The endpoint cache primary goal is to be able to provide the necessary data to
initiate a XGVMI operation, i.e., access a remote application's process memory from
a DPU. To illustrate this goal, one may concider the following intent:

**From DPU number *i*, initiate a XGVMI memory read from the first service process 
associated to rank *n* in the current of the current communicator at address *X*.**

To support this type of operation, we provied an endpoint cache, which makes available
the map service process/rank on all service and application processes.

The endpoint cache is composed of group caches. Group caches are composed of a rank 
cache with an entry per rank in the group. A cache entry provides all the data 
necessary to communicate with the rank, as well as a list of *shadow service 
processes*, i.e., the service processes that the rank is directly connected to and
that are on the same node.

Because group information is known only at run time, group caches are populated in a 
lazy way when ranks are connecting to a service process. When a rank connects, the 
cache is initialized if the group and the rank entry do not already exist, i.e., 
it is the first rank to connect. Of course, this assumes that the data passed in is 
accurate, i.e., the group identifier, rank and group size are the actual 
value and won't change over time.

## Bootstrapping

As mentioned earlier, the groups and ranks in the groups are only know at the time.
So in the context of an application relying on MPI for example, we use the following
data during the bootstrapping of the service, which is used to populate the endpoint
cache:
- the group identifier; in the context of MPI, it is the communicator identifier,
- the group size,
- the rank in the group,
- in the context of the group, the sub-group of the ranks on the local host (called *node group*), and more precisely, the rank in the node group and the node group size.

That data is sent to the service process during bootstrapping and directly added to
its local endpoint cache. Note that when the first rank of the group connects, it
implicitely initialize the associated group cache.

The service process, upon connection of the first rank, can calculate how many ranks
are expected to connect. When all the ranks are connected, the service processes
broadcast the cache entries of the ranks that are connected.

Once all the service processes broadcast their cache entries, all the service 
processes have a fully populated cache, and then send the full cache to the local 
ranks that are connected.

Upon global completion, all the ranks in the group and all the service processes  
have a fully populated endpoint cache for the group.


## Technical aspects

An endpoint cache is an array (`dyn_array_t`) of group caches. So a group identifier
is also the index in the array.
Each group cache as a vector of ranks (`dyn_array_t`) so the rank in the group, is
also the index in that array. In other words, each group gets its own rank cache.
And the final structure for that rank is a vector of service processes to define which SPs the rank is associated with.

Ultimately, based on a group and a rank, all the rank data including the shadow 
service processes are accessible via 2 array accesses.
