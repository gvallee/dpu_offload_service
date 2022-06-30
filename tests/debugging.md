# Testing and debugging

Here are some notes to test and debugging the offloading infrastructure code.

## Notifications

### Self-notifications

The `self_comm` is available to exercise self-notifications. It is meant to be executed on a single
DPU and the `DPU_OFFLOAD_LIST_DPUS` and `OFFLOAD_CONFIG_FILE_PATH` are expected to be set.
```
$ export DPU_OFFLOAD_LIST_DPUS=bluefield001
$ export OFFLOAD_CONFIG_FILE_PATH=/path/config/file.cfg
$ ./tests/comms/self_comm
```

It is also very easy to check for memory leaks while using notifications with Valgrind:
```
$ valgrind --log-file=./self_comm.log --leak-check=full --show-leak-kinds=all ./tests/comms/.libs/lt-self_comm
```

To find memory leaks in the context of a full job (not only self notifications), it is possible
to execute a run such as:
```
mpirun \
--hostfile $HOSTFILE \
--np 2 \
--map-by ppr:40:node \
--bind-to core \
--rank-by core \
--display bind \
--mca pml ucx \
-x UCX_NET_DEVICES=mlx5_4:1 \
-x UCX_TLS=rc_x \
-x OFFLOAD_CONFIG_FILE_PATH \
valgrind --leak-check=full --show-reachable=yes --log-file=a2av_ext.vg.%p \
/global/home/users/geoffroy/scratch/projects/dpu_offload/x86_64/ucc-priv/tools/perf/.libs/lt-ucc_perftest -c alltoallv_ext -F -n 1 -w 0 -b 64 -e 64
```

Then use `grep` to isolate output specific to the offloading library:
```
grep "dpu_offload_" a2av_ext.vg.3163914
```