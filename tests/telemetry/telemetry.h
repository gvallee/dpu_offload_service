//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef TELEMETRY_EXAMPLE_H
#define TELEMETRY_EXAMPLE_H

#define TELEMETRY_EXAMPLE_BASE_NOTIF_ID (2001)
#define LOAD_AVG_TELEMETRY_NOTIF (TELEMETRY_EXAMPLE_BASE_NOTIF_ID + 1)

// CMD_OUTPUT_BUF_SIZE is the maximum length of a single output.
#define CMD_OUTPUT_BUF_SIZE (2096)

typedef struct cmd_output {
    size_t size;
    size_t max_size;
    char output[CMD_OUTPUT_BUF_SIZE];
} cmd_output_t;

#define RESET_CMD_OUTPUT(_co)                   \
    do                                          \
    {                                           \
        (_co)->size = 0;                        \
        (_co)->max_size = CMD_OUTPUT_BUF_SIZE;  \
    } while (0)

#endif // TELEMETRY_EXAMPLE_H