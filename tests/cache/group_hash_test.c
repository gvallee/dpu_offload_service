//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <ucs/datastruct/khash.h>

#include <dpu_offload_types.h>

int main(int argc, char **argv)
{
    int *group;
    size_t i;

    group = (int *)malloc(1024);
    assert(group);

    // Simulate a contiguous group of 10 ranks (ranks 0-10)
    for (i = 0; i < 10; i++)
    {
        group[i] = i;
    }
    unsigned int key1 = HASH_GROUP(group, 10);
    fprintf(stdout, "hash for group of 10 = 0x%x\n", key1);
    unsigned int key2 = HASH_GROUP(group, 5);
    fprintf(stdout, "hash for a sub-group of 5 = 0x%x\n", key2);


    // Simulate a contiguous group of 10 ranks (ranks 1-11)
    for (i = 0; i < 10; i++)
    {
        group[i] = i + 1;
    }
    unsigned int key3 = HASH_GROUP(group, 10);
    fprintf(stdout, "hash for group of another 10 = 0x%x\n", key3);

    // Simulate a non-contiguous group of 10 ranks (ranks 0-8 and 42)
    for (i = 0; i < 10; i++)
    {
        group[i] = i;
    }
    group[9] = 42;
    unsigned int key4 = HASH_GROUP(group, 10);
    fprintf(stdout, "hash for group of another 10 = 0x%x\n", key4);

    free(group);

    return EXIT_SUCCESS;
}