#!/bin/sh
#

# -*- shell-script -*-
#
# Copyright 2022 NVIDIA CORPORATIONS. All rights reserved.
#
# See COPYING in top-level directory.
#
# Additional copyrights may follow
#
# $HEADER$
#

for i in 0 1 2 3 4 5 6 7
do
    echo "./fake_mpi_rank 127.0.0.1 8888 1 8 $i &"
    ./fake_mpi_rank 127.0.0.1 8888 1 8 $i &
done