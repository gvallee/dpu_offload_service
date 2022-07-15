//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "dynamic_structs.h"

int main(int argc, char **argv)
{
    smart_buffers_t smart_buffer_system;

    // Initialize the smart buffer system with default configuration
    SMART_BUFFS_INIT(&smart_buffer_system, NULL);
    fprintf(stderr, "INIt DONE\n");

    // Finalize the smart buffer system
    SMART_BUFFS_FINI(&smart_buffer_system);

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
}