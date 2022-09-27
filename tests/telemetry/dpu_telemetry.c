//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dpu_offload_service_daemon.h"
#include "dpu_offload_event_channels.h"
#include "telemetry.h"

#define DEFAULT_NUM_ELTS_IN_POOL (10)

typedef struct send_tracker {
    ucs_list_link_t item;

    // Meta event to track all the single sends to all the clients
    dpu_offload_event_t *meta_ev;

    // Associated command output that can only be freed once all the sends succeeded
    cmd_output_t cmd_output;
} send_tracker_t;

typedef void (*output_processing_fn_t) (cmd_output_t *);

typedef struct cmd_thread_params {
    // Command to execute
    char *cmd;

    // Optional function to process the command output
    // The function is responsibel ffor swappig the content of the cmd_output_t->output structure without memory leak
    output_processing_fn_t output_processing_fn;

    // Interval between command execution in microseconds
    uint64_t interval;

    // Execution context to use to execute the command; used to forward the output to the client(s)
    execution_context_t *econtext;

    // Type of the notification to use to forward the output result to the client(s)
    uint64_t notification_type;

    // List of ongoing sends so we can track send completion (and free resources) since all notifications are asynchronous
    simple_list_t ongoing_sends;
} cmd_thread_params_t;

#define RESET_CMD_THREAD_PARAMS(_p)                 \
    do                                              \
    {                                               \
        (_p)->cmd = NULL;                           \
        (_p)->output_processing_fn = NULL;          \
        (_p)->interval = 0;                         \
        (_p)->econtext = NULL;                      \
        (_p)->notification_type = 0;                \
        SIMPLE_LIST_INIT(&((_p)->ongoing_sends));   \
    } while (0)


int run_cmd(char *cmd, cmd_output_t *cmd_output)
{
    FILE *fp = NULL;
    char line[CMD_OUTPUT_BUF_SIZE];
    size_t max_size = cmd_output->max_size;

    assert(cmd_output);
    RESET_CMD_OUTPUT(cmd_output);
    assert(cmd_output->output);

    fp = popen(cmd, "r");
    assert(fp);

    while (fgets(line, sizeof(max_size), fp) != NULL)
    {
        char *ptr = (char*)((ptrdiff_t)cmd_output->output + cmd_output->size);
        strcpy(ptr, line);
        max_size -= strlen(line);
        cmd_output->size += strlen(line);
    }

    return 0;
}

// Starts a thread that periodically executes a command
void * cmd_exec_thread(void *arg)
{
    cmd_thread_params_t *params = NULL;
    dpu_offload_status_t rc;
    dyn_list_t *send_tracker_pool = NULL;

    assert(arg);
    params = (cmd_thread_params_t*) arg;
    assert(params->cmd);
    assert(params->econtext);
    assert(params->econtext->type == CONTEXT_SERVER);

    DYN_LIST_ALLOC(send_tracker_pool, DEFAULT_NUM_ELTS_IN_POOL, send_tracker_t, item);

    while (!EXECUTION_CONTEXT_DONE(params->econtext))
    {
        send_tracker_t *cur_send, *next_send;

        // Progress events that are aleady ongoing
        SIMPLE_LIST_FOR_EACH(cur_send, next_send, &(params->ongoing_sends), item)
        {
            if (event_completed(cur_send->meta_ev))
            {
                SIMPLE_LIST_DEL(&(params->ongoing_sends), &(cur_send->item));
                rc = event_return(&(cur_send->meta_ev));
                assert(rc == DO_SUCCESS);
            }
        }

        // If a client is connected, we send it the output
        if (params->econtext->server->connected_clients.num_connected_clients > 0)
        {
            dpu_offload_event_t *meta_ev = NULL;
            send_tracker_t *new_sends_tracker = NULL;
            size_t client;

            // Create a meta event to track all the sends to all the clients
            rc = event_get(params->econtext->event_channels, NULL, &meta_ev);
            assert(rc == DO_SUCCESS);
            assert(meta_ev);
            EVENT_HDR_TYPE(meta_ev) = META_EVENT_TYPE;
            DYN_LIST_GET(send_tracker_pool, send_tracker_t, item, new_sends_tracker);
            assert(new_sends_tracker);
            new_sends_tracker->meta_ev = meta_ev;

            // Run the command, get and process the output
            run_cmd(params->cmd, &(new_sends_tracker->cmd_output));
            if (params->output_processing_fn)
            {
                params->output_processing_fn(&(new_sends_tracker->cmd_output));
            }

            // Add the tracker to the list of ongoing operations so we can track completion overtime (everything is asynchronous)
            SIMPLE_LIST_PREPEND(&(params->ongoing_sends), &(new_sends_tracker->item));

            for (client = 0; client < params->econtext->server->connected_clients.num_connected_clients; client++)
            {
                dpu_offload_event_t *ev;
                dpu_offload_event_info_t ev_info;
                ucp_ep_h client_ep;

                RESET_EVENT_INFO(&ev_info);
                ev_info.explicit_return = true;
                rc = event_get(params->econtext->event_channels, &ev_info, &ev);
                assert(rc == DO_SUCCESS);
                ev->is_subevent = true;

                client_ep = GET_CLIENT_EP(params->econtext, client);
                if (client_ep == NULL)
                    fprintf(stderr, "Client %ld had no EP\n", client);
                assert(client_ep);
                rc = event_channel_emit_with_payload(&ev,
                                                     LOAD_AVG_TELEMETRY_NOTIF,
                                                     client_ep,
                                                     client,
                                                     NULL,
                                                     &(new_sends_tracker->cmd_output.output),
                                                     new_sends_tracker->cmd_output.size);
                if (rc != EVENT_DONE && rc != EVENT_INPROGRESS)
                {
                    fprintf(stderr, "event_channel_emit_with_payload() failed");
                    abort();
                }
                if (ev != NULL)
                {
                    // Event is still ongoing, we add it as a subevent to the meta-event
                    QUEUE_SUBEVENT(meta_ev, ev);
                }
            }
        }
        usleep(params->interval);
    }
    return NULL;
}

int main (int argc, char **argv)
{
    offloading_engine_t *offload_engine = NULL;
    offloading_config_t config_data;
    execution_context_t *service_server = NULL;
    cmd_thread_params_t load_avg_params;
    pthread_t tid_loadavg;
    dpu_offload_status_t rc;

    RESET_CMD_THREAD_PARAMS(&load_avg_params);
    load_avg_params.cmd = "cat /proc/loadavg";
    load_avg_params.interval = 5000000;
    load_avg_params.notification_type = LOAD_AVG_TELEMETRY_NOTIF;

    /*
     * BOOTSTRAPPING: WE CREATE A CLIENT THAT CONNECT TO THE INITIATOR ON THE HOST
     * AND INITIALIZE THE OFFLOADING SERVICE.
     */
    fprintf(stderr, "Creating offload engine...\n");
    rc = offload_engine_init(&offload_engine);
    if (rc || offload_engine == NULL)
    {
        fprintf(stderr, "offload_engine_init() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * GET THE CONFIGURATION.
     */
    fprintf(stderr, "Getting configuration...\n");
    INIT_DPU_CONFIG_DATA(&config_data);
    config_data.offloading_engine = offload_engine;
    int ret = get_dpu_config(offload_engine, &config_data);
    if (ret)
    {
        fprintf(stderr, "get_config() failed\n");
        return EXIT_FAILURE;
    }

    /*
     * INITIATE CONNECTION BETWEEN DPUS.
     */
    fprintf(stderr, "Initiating connections between DPUs\n");
    rc = inter_dpus_connect_mgr(offload_engine, &config_data);
    if (rc)
    {
        fprintf(stderr, "inter_dpus_connect_mgr() failed\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connections between DPUs successfully initialized\n");

    /* Wait for the DPUs to connect to each other */
    while (!all_service_procs_connected(offload_engine))
        offload_engine_progress(offload_engine);

    /*
     * CREATE A SERVER SO THAT PROCESSES RUNNING ON THE HOST CAN CONNECT.
     */
    fprintf(stderr, "Creating server for processes on the DPU\n");
    // We let the system figure out the configuration to use to let ranks connect
    service_server = server_init(offload_engine, &(config_data.local_service_proc.host_init_params));
    if (service_server == NULL)
    {
        fprintf(stderr, "service_server is undefined\n");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "server for application processes to connect has been successfully created\n");

    /*
     * START THE TELEMETRY SERVICE.
     */
    load_avg_params.econtext = service_server;
    pthread_create(&tid_loadavg, NULL, cmd_exec_thread, &load_avg_params);

    /*
     * PROGRESS UNTIL ALL PROCESSES ON THE HOST SEND A TERMINATION MESSAGE
     */
    fprintf(stderr, "%s: progressing...\n", argv[0]);
    while (!EXECUTION_CONTEXT_DONE(service_server))
    {
        lib_progress(service_server);
    }
    fprintf(stderr, "%s: server done, finalizing...\n", argv[0]);

    pthread_join(tid_loadavg, NULL);

    offload_engine_fini(&offload_engine);
    fprintf(stderr, "client all done, exiting successfully\n");

    return EXIT_SUCCESS;
}

