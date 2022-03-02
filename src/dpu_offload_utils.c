#define _POSIX_C_SOURCE 200809L

//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <string.h>
#include <stdio.h>
#include <stdio.h>
#include <assert.h>

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
dpu_offload_status_t config_version_1_parse_dpu_data(char *str, dpu_config_data_t *dpu_data)
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

static inline bool parse_dpu_cfg(char *dpu_hostname, char *str, char **hostname, char **addr, int *interdpu_conn_port, int *host_conn_port)
{
    char *token = strtok(token, ":");
    int step = 0;
    if (strncmp(token, dpu_hostname, strlen(token)))
    {
        // This is the DPU we are looking for
        *hostname = token;
        token = strtok(NULL, ":");
        while (token != NULL)
        {

            switch (step)
            {
            case 0:
                *addr = token;
                step++;
                break;
            case 1:
                *interdpu_conn_port = atoi(token);
                step++;
                break;
            case 2:
                *host_conn_port = atoi(token);
                return true;
            }
        }
    }
    return false;
}

// <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,...
// bool parse_line_dpu_version_1(int format_version, char *dpu_hostname, char *line, dpu_config_t **local_dpu_config, dyn_array_t *dpus, size_t *num_dpus_connecting_from)
bool parse_line_dpu_version_1(dpu_config_t *data, char *line)
{
    int idx = 0;
    bool rc = false;
    size_t num_dpus = 0;

    while (line[idx] == ' ')
        idx++;

    char *ptr = &(line[idx]);
    char *token = strtok(line, ",");

    // The host's name does not really matter here, moving to the DPU(s) configuration
    token = strtok(NULL, ",");
    assert(token);
    while (token != NULL)
    {
        int interdpu_conn_port, host_conn_port;
        char *addr = NULL;
        char *hostname = NULL;
        bool found = parse_dpu_cfg(data->local_dpu.hostname, token, &hostname, &addr, &interdpu_conn_port, &host_conn_port);

        dpu_config_data_t *dpu_config;
        DYN_ARRAY_GET_ELT(&(data->dpus_config), data->dpus_config.num_elts, dpu_config_data_t, dpu_config);
        assert(dpu_config);
        dpu_config->version_1.hostname = hostname;
        dpu_config->version_1.interdpu_port = interdpu_conn_port;
        dpu_config->version_1.rank_port = host_conn_port;
        dpu_config->version_1.addr = addr;
        data->dpus_config.num_elts++;

        // Warning, the order is important here or the logic will break
        // 1. If we already find the data for the local DPU, the current data is about a DPU we need to connect to.
        // 2. If the data is about the local data, we mark the DPU as being found; this prevents the next step to assume it needs to connect to itself.
        // 3. If the data is about a DPU before we found the local DPU, the local DPU will need to connect to it.
        if (data->dpu_found)
        {
            SET_DPU_TO_CONNECT_TO(data);
        }

        if (found)
        {
            // This is the DPU's configuration we were looking for
            data->dpu_found = true;
            data->local_dpu.config = dpu_config;
            rc = true;
        }

        if (!data->dpu_found)
            data->num_connecting_dpus++;

        token = strtok(NULL, ",");
    }
    return rc;
}

// <host name>,<dpu1_hostname:dpu_conn_addr:interdpu-port:rank-conn-port>,...
bool parse_line_version_1(char *target_hostname, dpu_config_t *data, char *line)
{
    int idx = 0;

    while (line[idx] == ' ')
        idx++;

    char *ptr = &(line[idx]);
    char *token = strtok(line, ",");
    DBG("Checking entry for %s", token);
    if (strncmp(token, target_hostname, strlen(token)) == 0)
    {
        // We found the hostname

        // Next tokens are the local DPUs' data
        // We get the DPUs configuration one-by-one.
        size_t dpu_idx = 0;
        token = strtok(NULL, ",");
        while (token != NULL)
        {
            dpu_config_data_t *dpu_config;
            DYN_ARRAY_GET_ELT(&(data->dpus_config), data->dpus_config.num_elts, dpu_config_data_t, dpu_config);
            assert(dpu_config);
            CHECK_ERR_RETURN((dpu_config == NULL), DO_ERROR, "unable to allocate resources for DPUs' configuration");

            dpu_offload_status_t rc = config_version_1_parse_dpu_data(token, dpu_config);
            CHECK_ERR_RETURN((rc), DO_ERROR, "config_version_1_parse_dpu_data() failed");
            data->dpus_config.num_elts++;
            token = strtok(NULL, ",");
        }
        DBG("%ld DPU(s) is/are specified for %s", data->dpus_config.num_elts, target_hostname);
        return true;
    }
    return false;
}

/**
 * @brief parse_line parses a line of the configuration file looking for a specific host name. It shall not be used to seek the configuration of a DPU.
 *
 * @param format_version Version of the format of the configuration file
 * @param target_hostname Host's name
 * @param line Line from the configuration file that is being parsed
 * @param config Configuration of the local host's DPU(s)
 * @return true when the line includes the host's configuration
 * @return false when the lines does not include the host's configuration
 */
bool parse_line(char *target_hostname, char *line, dpu_config_t *data)
{
    switch (data->format_version)
    {
    case 1:
        return parse_line_version_1(target_hostname, data, line);
    default:
        ERR_MSG("supported format (%s: version=%d)", line, data->format_version);
    }
    return false;
}

/**
 * @brief parse_line_for_dpu_cfg parses a line of the configuration file looking for a specific DPU. It shall not be used to seek the configuration of a host.
 *
 * @param format_version Version of the format of the configuration file
 * @param target_hostname DPU's hostname (assuming it is setup in separate mode)
 * @param line Line from the configuration file that is being parsed
 * @param config Configuration of the DPU
 * @return true when the line includes the host's configuration
 * @return false when the lines does not include the host's configuration
 */
bool parse_line_for_dpu_cfg(dpu_config_t *data, char *line)
{
    switch (data->format_version)
    {
    case 1:
        return parse_line_dpu_version_1(data, line);
    default:
        ERR_MSG("supported format (%s: version=%d)", line, data->format_version);
    }
    return false;
}

/**
 * @brief find_dpu_config_from_platform_configfile extracts a DPU's configuration from a platform configuration file.
 * It shall not be used to extract the configuration of a host.
 *
 * @param filepath Path the configuration file
 * @param config_data Object where all the configuration details are stored
 * @return dpu_offload_status_t
 */
dpu_offload_status_t find_dpu_config_from_platform_configfile(char *filepath, dpu_config_t *config_data)
{
    FILE *file = NULL;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    dpu_offload_status_t rc = DO_ERROR;
    bool first_line = true;

    file = fopen(filepath, "r");

    while ((read = getline(&line, &len, file)) != -1)
    {
        if (first_line)
        {
            rc = check_config_file_version(line, &(config_data->format_version));
            CHECK_ERR_GOTO((rc), error_out, "check_config_file_version() failed");
            CHECK_ERR_GOTO((config_data->format_version <= 0), error_out, "invalid version: %d", config_data->format_version);
            DBG("Configuration file based on format version %d", config_data->format_version);
            first_line = false;
            continue;
        }

        if (line_is_comment(line))
            continue;

        if (parse_line_for_dpu_cfg(config_data, line))
        {
            // We found the configuration for the DPU
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

/**
 * @brief find_config_from_platform_configfile extracts a host's configuration from a platform configuration file.
 * It shall not be used to extract the configuration of a DPU.
 *
 * @param filepath Path to the configuration file
 * @param hostname Name of the host to look up
 * @param data Configuration data of the host's local DPUs
 * @return dpu_offload_status_t
 */
dpu_offload_status_t find_config_from_platform_configfile(char *filepath, char *hostname, dpu_config_t *data)
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
            rc = check_config_file_version(line, &(data->format_version));
            CHECK_ERR_GOTO((rc), error_out, "check_config_file_version() failed");
            CHECK_ERR_GOTO((data->format_version <= 0), error_out, "invalid version: %d", data->format_version);
            DBG("Configuration file based on format version %d", data->format_version);
            first_line = false;
            continue;
        }

        if (line_is_comment(line))
            continue;

        if (parse_line(hostname, line, data))
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
