//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include "client_common.h"

int main(int argc, char **argv)
{
    // We let the library create a worker
    return run_client_test(NULL, NULL);
}