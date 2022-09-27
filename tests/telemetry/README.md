# On the DPU

```
export OFFLOAD_CONFIG_FILE_PATH=/path/to/dpu_offload_service/etc/platforms/jupiter.cfg
export DPU_OFFLOAD_LIST_DPUS=jupiterbf001
$ ./tests/telemetry/dpu_telemetry
```

# On the host

```
export DPU_OFFLOAD_SERVER_ADDR=192.168.130.101
export DPU_OFFLOAD_SERVER_PORT=9999
$ ./tests/telemetry/client_telemetry
```