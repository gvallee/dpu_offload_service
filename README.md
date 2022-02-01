# dpu_offload_service

## Requirements

The following packages and versions are required:
- UCX 1.12 or newer
- openpmix 4.1.0 or newer
- PRTE 2.0.0 or newerr

## Compilation

./autogen.sh && ./configure --with-pmix=<PMIX/INSTALL/DIR> --with-prrte=<PRRTE/INSTALL/DIR> --with-ucx=<UCX/INSTALL/DIR> && make