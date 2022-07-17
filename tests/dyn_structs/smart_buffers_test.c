//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "dynamic_structs.h"

static void DISPLAY_SMART_BUFFERS_SYS_DATA(smart_buffers_t *sys)
{
    size_t i;
    fprintf(stdout, "Number of buckets: %ld\n", sys->num_buckets);
    fprintf(stdout, "Number of underlying memory segments: %ld\n", sys->num_base_mem_chunks);
    fprintf(stdout, "List of buckets:\n");
    for (i = 0; i < sys->num_buckets; i++)
    {
        smart_bucket_t *b = DYN_ARRAY_GET_ELT(&(sys->buckets), i, smart_bucket_t);
        fprintf(stdout, "\tBucket %ld - min size: %ld, max size: %ld, number of elements: %ld\n",
                i, b->min_size, b->max_size, ucs_list_length(&(b->pool)));
    }
}

int main(int argc, char **argv)
{
    size_t i;
    smart_buffers_t smart_buffer_system;

    // Initialize the smart buffer system with default configuration
    SMART_BUFFS_INIT(&smart_buffer_system, NULL);

    // Print smart buffer system's info
    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);

    fprintf(stdout, "Getting and returning one smart chunk from each bucket\n");
    for (i = 0; i < smart_buffer_system.num_buckets; i++)
    {
        smart_bucket_t *bucket = DYN_ARRAY_GET_ELT(&(smart_buffer_system.buckets), i, smart_bucket_t);
        smart_chunk_t *sc = SMART_BUFF_GET(&smart_buffer_system, bucket->min_size + 1);
        assert(sc);
        fprintf(stdout, "\t-> Successfully got a smart buffer of size %ld\n", bucket->min_size + 1);
        SMART_BUFF_RETURN(&smart_buffer_system, bucket->min_size + 1, sc);
        assert(sc == NULL);
        fprintf(stderr, "\t\t-> now successfully returned\n");
    }

    fprintf(stdout, "Getting 10 smart chunks from the first smart bucket and checking the address of the buffers\n");
    ucs_list_link_t buffers;
    ucs_list_head_init(&buffers);
    void *addr = NULL;
    for (i = 0; i < 10; i++)
    {
        size_t queried_size = 1;
        smart_chunk_t *sc = SMART_BUFF_GET(&smart_buffer_system, queried_size);
        assert(sc);
        if (addr == NULL)
            addr = sc->base;
        else
            assert(sc->base == (void *)((ptrdiff_t)addr + (i * sc->bucket->max_size)));
    }
    fprintf(stdout, "\t-> ok\n");

    // Finalize the smart buffer system
    SMART_BUFFS_FINI(&smart_buffer_system);

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
}