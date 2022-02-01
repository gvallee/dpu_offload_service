//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dpu_offload_service_daemon.h"

int main(int argc, char **argv)
{
    dpu_offload_client_t *client;
    int rc = client_init(&client);
    if (rc)
    {
        fprintf(stderr, "init_client() failed\n");
        return EXIT_FAILURE;
    }

    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }

    client_fini(&client);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}