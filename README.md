# dpu_offload_service

## Requirements

The following packages and versions are required:
- UCX 1.12 or newer
- openpmix 4.1.0 or newer
- PRTE 2.0.0 or newer

## Compilation

./autogen.sh && ./configure --with-pmix=<PMIX/INSTALL/DIR> --with-prrte=<PRRTE/INSTALL/DIR> --with-ucx=<UCX/INSTALL/DIR> && make

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

