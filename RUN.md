# Manual start of the daemons on the DPUs using a configuration file (recommanded method)

## Format of the configuration file.

Please refer to [doc/configfile.md](./doc/configfile.md) for details.

## Execution of the DPU daemon

Assuming you want to use two BlueField cards, `thor-bf21` and `thor-bf22`, please execute the following on each DPU:
```
export OFFLOAD_CONFIG_FILE_PATH=`pwd`/etc/platforms/thor.cfg
export DPU_OFFLOAD_LIST_DPUS="thor-bf21,thor-bf22"
./daemons/job_persistent/job_persistent_dpu_daemon
```
The offloading service daemon will then start and DPUs will connect to each other.

[doc/configfile.md](./doc/configfile.md) includes details for the implementation of a script to start
multiple service processes per DPU.

# Manual start of the daemons on the DPUs using environment variables

Assuming the TCP out-of-band mechanism is used to bootstrap the infrastructure, please do the following steps on each DPU.

First, export the proper environment variable used to specify all the DPUs that will be used. For example:

```
export DPU_OFFLOAD_LIST_DPUS="thor-bf15.hpcadvisorycouncil.com,thor-bf16.hpcadvisorycouncil.com,thor-bf17.hpcadvisorycouncil.com"
```

Then, specify the address to use for the inter-DPU connections. For example:

```
export INTER_DPU_CONN_ADDR=192.168.131.123
```

Then, specify the port to use on the DPUs for the DPUs to connect to each other. For example:

```
export INTER_DPU_CONN_PORT=11110
```

Then, export the proper environment variable to let the ranks that will run on the host connect to the DPU offloading service. For example:

```
export DPU_OFFLOAD_SERVER_PORT=11111
```

Then, export the proper environment variable to set the address to use to let the ranks connect. For example:

```
export DPU_OFFLOAD_SERVER_ADDR=192.168.131.121
```

Finally, you can start the provided daemon:

```
./daemons/job_persistent/job_persistent_dpu_daemon
```

# Execution of an MPI application with collective offloading

For instructions to install UCC with DPU offloading, please refer to [INSTALL.md](./INSTALL.md).

The recommanded way to execute an application with offloaded collective is the following; the examples are based on MPI but applications using UCC directly can do similar things.

1. Create a configuration file for your platform. An example is in `etc/platforms/thor.cfg`.

2. Execute your application setting the environment variable `OFFLOAD_CONFIG_FILE_PATH` to the configuration file. The configuration file has all the required information to set the entire infrastructure and let the ranks connected to the service running on the DPUs. Also make sure your MPI implementation is using the UCC collective. In the context of Open MPI, it means the `--mca coll_ucc_enable 1 --mca coll_ucc_priority 100` must be added to the `mpirun` command.

Example:
```
mpirun -np 2 --mca coll_ucc_enable 1 --mca coll_ucc_priority 100 -x OFFLOAD_CONFIG_FILE_PATH=`pwd`/etc/platforms/thor.cfg app.exe
```
