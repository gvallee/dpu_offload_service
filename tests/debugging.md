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