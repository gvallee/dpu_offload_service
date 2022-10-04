# Overview

This test illustrates the creation of two services in the context of a single process on both the DPU and host.
In other words, the services are provided through two different engines running in a single process on the DPU;
and the services are used through two different engines in a single process running on the host.
The test also ensures that it is possible to use the same notification IDs on two different engines and have
correct deliveries of all notifications, i.e., they are delivered to the correct execution context on the
correct engine.

# Testing on a local machine

## Configuration files

Please update the configuration files in `tests/offload_service/multi_services/etc/` to match your system.

## DPU side

```
$ export DPU_OFFLOAD_LIST_DPUS=127.0.0.1
$ export MY_CFG_FILE_SERVICE1=`pwd`/tests/offload_service/multi_services/etc/local_test_service1.cfg
$ export MY_CFG_FILE_SERVICE2=`pwd`/tests/offload_service/multi_services/etc/local_test_service2.cfg
$ ./tests/offload_service/multi_services/two_engines_dpu
```

## Host side

```
$ export DPU_OFFLOAD_LIST_DPUS=127.0.0.1
$ export MY_CFG_FILE_SERVICE1=`pwd`/tests/offload_service/multi_services/etc/local_test_service1.cfg
$ export MY_CFG_FILE_SERVICE2=`pwd`/tests/offload_service/multi_services/etc/local_test_service2.cfg
$ ./tests/offload_service/multi_services/two_engines_host
```
