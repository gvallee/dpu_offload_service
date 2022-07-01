# Configuration file

The overall format of the configuration file is as follow:

```
# Format version: 1
# <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port1&interdpu-port2:rank-conn-port1&rank-conn-port2>,...
```

Note that the first comment specifying the version of the format being used is **mandatory**.
At the moment, only one format is supported.

A single configuration file can be used by platform; it does not need to be tailored to the job
configuration. In other words, when running a job, the infrastructure is capable of extracting
only the required data from the configuration file.

Examples of configuration files are available in `etc/platforms`.

Each line must describe a host and its associated DPU(s):

1. the first token must be the full hostname of the host followed by a `,` and
1. the list of DPUs to use; it is allowed to have a single DPU,
1. for each DPU:

        * The full DPU hostname, followed by the character ':'.
        * The IP address to use for boostrapping, followed by the character ':'.
        * A list of ports, separated by the & symbol and followed by the character ':', defining the ports to use for connections between service processes. Note it also specifies the number of service processes per DPU, one per port.
        * A list of ports for connections with processes running on the host. This list **must** have
        the same number of ports as the previous list.

## Start multiple service processes on DPUs using a configuration file

In this section, the configuration assumes 2 service processes per DPU.

The configuration file in this case, specifies 2 different ports, one per 
service process, for both inter-service-processes and host-service-process
configuration. For example:

```
# Format version: 1
# <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port1&interdpu-port2:rank-conn-port1&rank-conn-port2>,...
node001.hpcadvisorycouncil.com,nodebf001.hpcadvisorycouncil.com:192.168.129.101:7010&7011:9010&9011
node002.hpcadvisorycouncil.com,nodebf002.hpcadvisorycouncil.com:192.168.129.102:7010&7011:9010&9011
node003.hpcadvisorycouncil.com,nodebf003.hpcadvisorycouncil.com:192.168.129.103:7010&7011:9010&9011
node004.hpcadvisorycouncil.com,nodebf004.hpcadvisorycouncil.com:192.168.129.104:7010&7011:9010&9011
node005.hpcadvisorycouncil.com,nodebf005.hpcadvisorycouncil.com:192.168.129.105:7010&7011:9010&9011
```

Note that the hosts are named `hostXXX` and their associated BlueField cards 
`hostbfXXX`. Also note that based on this configuration file, port 7010 and 
7010 will be used for inter-service-processes communications (2 per DPU) and 
9010 and 9011 for communications with the host (respectively by the local 
service process 0 and 1).

The script to start the service processes on DPUs look like:

```
#!/bin/bash
#

CFG_FILE=/global/home/joedoe/dpu_offload/etc/platforms/myplatform.cfg
DAEMON_EXE=/global/home/joedoe/dpu_offload/arm64/offload_dpu_daemon
SPS_PER_DPU=2

function start_dpu_daemon () {
GLOBAL_SP_ID=0
for DPU in "${DPUS[@]}"; do
        for LOCAL_SP_ID in $(seq 0 $((SPS_PER_DPU-1))); do
                LOCAL_SP_ID=$(($GLOBAL_SP_ID % $SPS_PER_DPU))
                echo "-> Starting SP with GID $GLOBAL_SP_ID and LID $LOCAL_SP_ID deamons on $DPU ..."
                CMD="DPU_OFFLOAD_SERVICE_PROCESS_GLOBAL_ID=$GLOBAL_SP_ID DPU_OFFLOAD_SERVICE_PROCESS_LOCAL_ID=$LOCAL_SP_ID DPU_OFFLOAD_SERVICE_PROCESSES_PER_DPU=$SPS_PER_DPU DPU_OFFLOAD_LIST_DPUS=$DPU_OFFLOAD_LIST_DPUS OFFLOAD_CONFIG_FILE_PATH=$CFG_FILE LD_LIBRARY_PATH=/global/home/joedoe/dpu_offload/arm64/install/ucx-xgvmi/lib:/global/home/joedoe/dpu_offload/arm64/install/dpu_offload_service/lib:$LD_LIBRARY_PATH UCX_NET_DEVICES=mlx5_0:1 UCX_TLS=rc_x UCX_ZCOPY_THRESH=0  $DAEMON_EXE"
                echo "--> Executing $CMD..."
                ssh $DPU "$CMD" &
                GLOBAL_SP_ID=$(($GLOBAL_SP_ID+1))
        done
done
}

export DPU_OFFLOAD_LIST_DPUS="nodebf001,nodebf002,nodebf003,nodebf004,nodebf005,nodebf006,nodebf007,nodebf008,nodebf009,nodebf010,nodebf011,nodebf012,nodebf013,nodebf014,nodebf015,nodebf016"

start_dpu_daemon
```

Note that the example points to the location where UCX is installed.