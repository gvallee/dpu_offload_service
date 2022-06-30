# UCC integration

The offloading library has been integrated with UCC in order to offload collective operations.
This document aims at giving high-level description of how the integration was done.

## UCC team

UCC teams ultimately represent MPI communciators to perform a collective operations.
As a result, in order to perform the offloading, all ranks in a team need to connect
to a service process running on a local DPU that implements the offloading capability.

Since it is possible to have multiple service processes per DPU and to support XGVMI,
we need to create a map of which rank is connected to which service process.
Since that mapping, called endpoint cache, needs to be distributed amongst all the
service processes and all the ranks in the group, one of the difficulties is to know
when all the ranks that are supposed to connect to a given service process are ready
so the service processes can exchange their cache entries for their ranks with other
service processes. Once that exchange completes, all service processes have a complete
endpoint cache, which can then be pushed to the local ranks running on the host.

To implement this, we currently implement the following semantics:

1. UCC is requested to know how many ranks are running on the host. For that, we create a topology for the local node during the team creation (UCC provides the infrastructure for this but does not generate this topology by default).
1. Based of the number of local ranks and the local rank of a process, the process queries the offloading library to know which local service process it is assigned to.
1. Bootstrapping with the assigned service process is initiated. During the bootstrapping phase, the rank always sends the team identifier from UCC, the team size, the number of local ranks in the team and its local node-rank.
1. Since the service process receives the data about the team and the number of ranks on the host, it can determine how many ranks are expected to connect.
1. Once all expected ranks are connected, the service process uses a notification to broadcast the cache entries related to the local ranks to other service processes. Since notifications are fully asynchronous, service processes can also receive cache entries at any given time, populating the endpoint cache.
1. When a service process has a fully populated endpoint cache, it sends it to all the local ranks that are connected.

Finally, note that the current implementation starts offloading operations only when
the endpoint cache is fully populated, mainly to ensure that all the data required
to perform any XGVMI operations between any rank and any service process is possible.
In other words, the current implementation has a synchronization point on the host to
make sure that the cache is populated, which also implies that the cache is also
virtually populated on all service processes.