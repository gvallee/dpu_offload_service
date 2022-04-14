# Requirements

The following packages and versions are required:
- UCX 1.12 or newer
  
The following pachages are optional when there is no need to remotely start binaries on DPUs:
- openpmix 4.1.0 or newer
- PRTE 2.0.0 or newer

# Compilation & installation

## Standard mode

For standard configurations:
```
./autogen.sh && ./configure --prefix=<PREFIX> --with-ucx=<UCX/INSTALL/DIR> && make -j install
```

With PMIx and PRRTE support:
```
./autogen.sh && ./configure --prefix=<PREFIX> --with-pmix=<PMIX/INSTALL/DIR> --with-prrte=<PRRTE/INSTALL/DIR> --with-ucx=<UCX/INSTALL/DIR> && make -j install
```

## Debug mode

Simply add the `--enable-debug` configuration option. For example:

```
./autogen.sh && ./configure --prefix=<PREFIX> --with-ucx=<UCX/INSTALL/DIR> --enable-debug && make -j install
```

Note that this also enables tracing, developers will be able to see debug messages for the main capabilities of the library.

# UCC integration for collective offloading

To compile a UCC version compatible with DPU offloading (e.g., [ucc-priv](https://github.com/yqin/ucc-priv)), add the `--with-dpu-offload=<DIR>` option while configuring UCC. This option will check whether the DPU offloading libraries have been correctly install, and if so, enable the offloading code and ensure the UCC code can be compiled correctly.

For instructions for the execution of applications using offloaded collectives, please refer to [RUN.md](./RUN.md).
