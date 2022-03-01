# dpu_offload_service

## Requirements

The following packages and versions are required:
- UCX 1.12 or newer
- openpmix 4.1.0 or newer
- PRTE 2.0.0 or newer

## Compilation

### Standard mode

```
./autogen.sh && ./configure --prefix=<PREFIX> --with-pmix=<PMIX/INSTALL/DIR> --with-prrte=<PRRTE/INSTALL/DIR> --with-ucx=<UCX/INSTALL/DIR> && make -j install
```

### Debug mode

```
./autogen.sh && ./configure --prefix=<PREFIX> --with-pmix=<PMIX/INSTALL/DIR> --with-prrte=<PRRTE/INSTALL/DIR> --with-ucx=<UCX/INSTALL/DIR> --enable-debug && make -j install
```

Note that this also enables tracing, developers will be able to see debug messages for the main capabilities of the library.

## Execution

### Manually start the daemons on the DPUs

Assuming the TCP out-of-band mechanism is used to bootstrap the infrastructure, please do the following steps on each DPU.

First, export the proper environment variable used to specify all the DPUs that will be used. For example:

export DPU_OFFLOAD_LIST_DPUS="thor-bf15.hpcadvisorycouncil.com,thor-bf16.hpcadvisorycouncil.com,thor-bf17.hpcadvisorycouncil.com"

Then, specify the address to use for the inter-DPU connections. For example:

export INTER_DPU_CONN_ADDR=192.168.131.123

Then, specify the port to use on the DPUs for the DPUs to connect to each other. For example:

export INTER_DPU_CONN_PORT=11110

Then, export the proper environment variable to let the ranks that will run on the host connect to the DPU offloading service. For example:

export DPU_OFFLOAD_SERVER_PORT=11111

Then, export the proper environment variable to set the address to use to let the ranks connect. For example:

export DPU_OFFLOAD_SERVER_ADDR=192.168.131.121

Finally, you can start the provided daemon:

./daemons/job_persistent/job_persistent_dpu_daemon

### Start an MPI application with collective offloading

Assuming a version of UCC supported DPU offloading has been installed, i.e., successfully configured with the `--with-dpu-offload=<DIR>` option, and installed, the recommanded way to execute an application is the following. Examples are based on MPI but applications using UCC directly can do similar things.

1. Create a configuration file for your platform. An example is in `etc/platforms/thor.cfg`.

2. Execute your application setting the environment variable `OFFLOAD_CONFIG_FILE_PATH` to the configuration file. The configuration file has all the required information to set the entire infrastructure and let the ranks connected to the service running on the DPUs. Also make sure your MPI implementation is using the UCC collective. In the context of Open MPI, it means the `--mca coll_ucc_enable 1 --mca coll_ucc_priority 100` must be added to the `mpirun` command.

Example:
```
mpirun -np 2 --mca coll_ucc_enable 1 --mca coll_ucc_priority 100 -x OFFLOAD_CONFIG_FILE_PATH=`pwd`/etc/platforms/thor.cfg simple_alltoallv
```