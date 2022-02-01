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
    dpu_offload_server_t *server;
    int rc = server_init(&server);
    if (rc)
    {
        fprintf(stderr, "init_server() failed\n");
        return EXIT_FAILURE;
    }
    if (server == NULL)
    {
        fprintf(stderr, "server handle is undefined\n");
        return EXIT_FAILURE;
    }

    while (!server->done)
    {
        server_progress(server);
    }

    server_fini(&server);

    fprintf(stderr, "server all done, exiting successfully\n");
    return EXIT_SUCCESS;
}