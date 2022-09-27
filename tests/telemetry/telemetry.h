//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#ifndef TELEMETRY_EXAMPLE_H
#define TELEMETRY_EXAMPLE_H

#define TELEMETRY_EXAMPLE_BASE_NOTIF_ID (2001)
#define LOAD_AVG_TELEMETRY_NOTIF (TELEMETRY_EXAMPLE_BASE_NOTIF_ID + 1)

typedef struct cmd_output {
    char **output;
    size_t num;
} cmd_output_t;

#define RESET_CMD_OUTPUT(_co)   \
    do                          \
    {                           \
        (_co)->output = NULL;   \
        (_co)->num = 0;         \
    } while (0)

#endif // TELEMETRY_EXAMPLE_H