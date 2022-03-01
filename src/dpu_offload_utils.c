#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <string.h>
#include <stdio.h>

#include "dpu_offload_types.h"
#include "dpu_offload_mem_mgt.h"
#include "dpu_offload_debug.h"
#include "dpu_offload_event_channels.h"
#include "dpu_offload_envvars.h"



const char *config_file_version_token = "Format version:";

int send_cache_entry(execution_context_t *econtext, ucp_ep_h ep, peer_cache_entry_t *cache_entry)
{
    dpu_offload_event_t *send_cache_entry_ev;
    dpu_offload_status_t rc = event_get(econtext->event_channels, &send_cache_entry_ev);
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_get() failed");

    rc = event_channel_emit(send_cache_entry_ev,
                            ECONTEXT_ID(econtext),
                            AM_PEER_CACHE_ENTRIES_MSG_ID,
                            ep,
                            NULL,
                            &(cache_entry->peer),
                            sizeof(cache_entry->peer));
    CHECK_ERR_RETURN((rc), DO_ERROR, "event_channel_emit() failed");

    // Put the event on the ongoing events list used while progressing the execution context.
    // When event complete, we can safely return them.
    ucs_list_add_tail(&(econtext->ongoing_events), &(send_cache_entry_ev->item));

    return DO_SUCCESS;
}

dpu_offload_status_t check_config_file_version(char *line, int *version)
{
    int idx = 0;

    // Skip heading spaces to find the first valid character
    while (line[idx] == ' ')
        idx++;

    // First valid character must be '#'
    CHECK_ERR_RETURN((line[idx] != '#'), DO_ERROR, "First line of config file does not start with #");
    idx++;

    // Then, first valid token must be 'Format version:'
    while (line[idx] == ' ')
        idx++;
    char *token = &(line[idx]);
    if (strncmp(token, config_file_version_token, strlen(config_file_version_token)) != 0)
    {
        ERR_MSG("First line does not include the version of the format (does not include %s)", config_file_version_token);
        return DO_ERROR;
    }
    idx += strlen(config_file_version_token);

    // Then we should have the version number
    while (line[idx] == ' ')
        idx++;
    token = &(line[idx]);
    *version = atoi(token);
    return DO_SUCCESS;
}

bool line_is_comment(char *line)
{
    int idx = 0;
    while (line[idx] == ' ')
        idx++;
    if (line[idx] == '#')
        return true;
    return false;
}

// <dpu_hostname:interdpu-port:rank-conn-port>
dpu_offload_status_t config_version_1_parse_dpu_data(char *str, dpu_config_t *dpu_data)
{
    CHECK_ERR_RETURN((str == NULL), DO_ERROR, "undefined string");
    CHECK_ERR_RETURN((dpu_data == NULL), DO_ERROR, "undefined DPU data structure");

    int step = 0;
    char *token = strtok(str, ":");
    while (token != NULL)
    {
        switch (step)
        {
        case 0:
            dpu_data->version_1.hostname = strdup(token);
            break;
        case 1:
            dpu_data->version_1.addr = strdup(token);
            break;
        case 2:
            dpu_data->version_1.interdpu_port = atoi(token);
            break;
        case 3:
            dpu_data->version_1.rank_port = atoi(token);
            break;
        }

        step++;
        token = strtok(NULL, ":");
    }
    return DO_SUCCESS;
}

// <host name>,<dpu1:interdpu-port:rank-conn-port>,...
bool parse_line_version_1(int format_version, char *target_hostname, char *line, dpu_config_t **config)
{
    int idx = 0;

    // For now we do not try to be smart and optimize things: first figure out how many DPUs are defined
    size_t num_dpus = 0;
    size_t line_idx = 0;
    while (line_idx != strlen(line))
    {
        if (line[line_idx] == ',')
            num_dpus++;
        line_idx++;
    }
    DBG("%ld DPU(s) is/are specified for %s", num_dpus, target_hostname);

    while (line[idx] == ' ')
        idx++;

    char *ptr = &(line[idx]);
    char *token = strtok(line, ",");
    DBG("Checking entry for %s", token);
    if (strncmp(token, target_hostname, strlen(token)) == 0)
    {
        // We found the hostname

        // Next tokens are the local DPUs' data
        dpu_config_t *dpus_config = calloc(num_dpus, sizeof(dpu_config_t));
        CHECK_ERR_RETURN((dpus_config == NULL), DO_ERROR, "unable to allocate resources for DPUs' configuration");
        *config = dpus_config;

        // Then get the DPUs configuration one-by-one.
        size_t dpu_idx = 0;
        token = strtok(NULL, ",");
        while (token != NULL)
        {
            dpu_offload_status_t rc = config_version_1_parse_dpu_data(token, &(dpus_config[dpu_idx]));
            CHECK_ERR_RETURN((rc), DO_ERROR, "config_version_1_parse_dpu_data() failed");
            token = strtok(NULL, ",");
            dpu_idx++;
        }
        return true;
    }
    return false;
}

bool parse_line(int format_version, char *target_hostname, char *line, dpu_config_t **config)
{
    switch (format_version)
    {
    case 1:
        return parse_line_version_1(format_version, target_hostname, line, config);
    default:
        ERR_MSG("supported format (%s: version=%d)", line, format_version);
    }
    return false;
}

dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, dpu_config_t **config)
{
    FILE *file = NULL;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    dpu_offload_status_t rc = DO_ERROR;
    bool first_line = true;
    int version = 0;

    file = fopen(filepath, "r");

    while ((read = getline(&line, &len, file)) != -1)
    {
        if (first_line)
        {
            rc = check_config_file_version(line, &version);
            CHECK_ERR_GOTO((rc), error_out, "check_config_file_version() failed");
            CHECK_ERR_GOTO((version <= 0), error_out, "invalid version: %d", version);
            DBG("Configuration file based on format version %d", version);
            first_line = false;
            continue;
        }

        if (line_is_comment(line))
            continue;

        if (parse_line(version, hostname, line, config))
        {
            // We found the configuration for the hostname
            break;
        }
    }

    rc = DO_SUCCESS;

error_out:
    fclose(file);
    if (line)
        free(line);

    return rc;
}

dpu_offload_status_t get_env_config(conn_params_t *params)
{
    char *server_port_envvar = getenv(SERVER_PORT_ENVVAR);
    char *server_addr = getenv(SERVER_IP_ADDR_ENVVAR);
    int port = -1;

    CHECK_ERR_RETURN((!server_addr), DO_ERROR,
                     "Invalid server address, please make sure the environment variable %s or %s is correctly set", SERVER_IP_ADDR_ENVVAR, INTER_DPU_ADDR_ENVVAR);

    if (server_port_envvar)
    {
        port = (uint16_t)atoi(server_port_envvar);
    }

    CHECK_ERR_RETURN((port < 0), DO_ERROR, "Invalid server port (%s), please specify the environment variable %s",
                     server_port_envvar, SERVER_PORT_ENVVAR);

    params->addr_str = server_addr;
    params->port_str = server_port_envvar;
    params->port = port;

    return DO_SUCCESS;
}
