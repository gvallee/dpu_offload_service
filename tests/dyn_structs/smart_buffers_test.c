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

#define SMART_BUFF_NUM_GETS (10000)

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
                i, b->min_size, b->max_size, SIMPLE_LIST_LENGTH(&(b->pool)));
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

    fprintf(stdout, "Getting %d smart chunks from the first smart bucket and checking the address of the buffers\n", SMART_BUFF_NUM_GETS);
    ucs_list_link_t buffers;
    ucs_list_head_init(&buffers);
    size_t queried_size = 1;
    void *addr = NULL;
    for (i = 0; i < SMART_BUFF_NUM_GETS; i++)
    {
        smart_chunk_t *sc = SMART_BUFF_GET(&smart_buffer_system, queried_size);
        assert(sc);
        if (addr == NULL)
            addr = sc->base;
        else
            assert(sc->base == (void *)((ptrdiff_t)addr + (i * sc->bucket->max_size)));
        ucs_list_add_tail(&buffers, &(sc->super));
    }
    while (!ucs_list_is_empty(&buffers))
    {
        smart_chunk_t *chunk = ucs_list_extract_head(&buffers, smart_chunk_t, super);
        assert(chunk);
        SMART_BUFF_RETURN(&smart_buffer_system, queried_size, chunk);
        assert(chunk == NULL);
    }
    fprintf(stdout, "\t-> ok\n");

    fprintf(stdout, "Getting an element in the last bucket\n");
    size_t *last_bucket_size = DYN_ARRAY_GET_ELT(&(smart_buffer_system.bucket_sizes), smart_buffer_system.num_buckets - 1, size_t);
    queried_size = *last_bucket_size + 1;
    smart_chunk_t *beyong_last_bucket_sc = SMART_BUFF_GET(&smart_buffer_system, queried_size);
    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);
    fprintf(stdout, "\t-> ok\n");

    // Finalize the smart buffer system
    SMART_BUFFS_FINI(&smart_buffer_system);

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
}