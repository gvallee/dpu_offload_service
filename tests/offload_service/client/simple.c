//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

/*
 * Simple is a basic client that connects to a server (including DPU daemon),
 * sleeps for 5 seconds and terminates. It is for example used to test the
 * connection of multiple clients to a single DPU daemon.
 */

#include <unistd.h>

#include "dpu_offload_service_daemon.h"

int main(int argc, char **argv)
{
    execution_context_t *client = NULL;
    offloading_engine_t *offload_engine = NULL;
    dpu_offload_status_t rc;

    /* Initialize everything we need for the test */
    rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        goto error_out;
    }

    client = client_init(offload_engine, NULL);
    if (client == NULL)
    {
        fprintf(stderr, "client handle is undefined\n");
        return EXIT_FAILURE;
    }
    ADD_CLIENT_TO_ENGINE(client, offload_engine);

    // Waiting until the client is fully connected
    ECONTEXT_LOCK(client);
    int client_state = client->client->bootstrapping.phase;
    ECONTEXT_UNLOCK(client);
    while (client_state != BOOTSTRAP_DONE)
    {
        lib_progress(client);
        ECONTEXT_LOCK(client);
        client_state = client->client->bootstrapping.phase;
        ECONTEXT_UNLOCK(client);
    }

    fprintf(stderr, "* Now connected to the server\n");

    // We are now connected to the server
    sleep(5);

    // All done, terminating
    client_fini(&client);
    offload_engine_fini(&offload_engine);
    fprintf(stdout, "%s: test successful\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    if (client != NULL)
        client_fini(&client);
    if (offload_engine != NULL)
        offload_engine_fini(&offload_engine);
    fprintf(stderr, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;

}