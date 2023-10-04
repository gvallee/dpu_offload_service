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
        size_t list_len = SIMPLE_LIST_LENGTH(&(b->pool));
        fprintf(stdout, "\tBucket %ld - min size: %ld, max size: %ld, number of elements: %ld\n",
                i, b->min_size, b->max_size, list_len);
        assert(list_len >= 0);
    }
}

int get_return_one_from_each_bucket(smart_buffers_t *smart_buffer_system)
{
    size_t i;

    fprintf(stdout, "Getting %d smart chunks from the first smart bucket and checking the address of the buffers\n", SMART_BUFF_NUM_GETS);
    ucs_list_link_t buffers;
    ucs_list_head_init(&buffers);
    size_t queried_size = 1;
    void *addr = NULL;
    uint64_t expected_smart_chunk_seq_num = 0;
    for (i = 0; i < SMART_BUFF_NUM_GETS; i++)
    {
        smart_chunk_t *sc = SMART_BUFF_GET(smart_buffer_system, queried_size);
        assert(sc);
        // We always expect to get chunk in the original order of the buckets
        assert(sc->seq_num == expected_smart_chunk_seq_num);
        if (addr == NULL)
        {
            addr = sc->base;
        }
        else
        {
            // The list adds and points to the tail
            assert(sc->base == (void *)((ptrdiff_t)addr + (i * sc->bucket->max_size)));
        }
        expected_smart_chunk_seq_num++;
        ucs_list_add_head(&buffers, &(sc->super));
    }
    while (!ucs_list_is_empty(&buffers))
    {
        smart_chunk_t *chunk = ucs_list_extract_head(&buffers, smart_chunk_t, super);
        assert(chunk);
        SMART_BUFF_RETURN(smart_buffer_system, queried_size, chunk);
        assert(chunk == NULL);
    }

    fprintf(stdout, "Getting and returning one smart chunk from each bucket\n");
    for (i = 0; i < smart_buffer_system->num_buckets; i++)
    {
        smart_bucket_t *bucket = DYN_ARRAY_GET_ELT(&(smart_buffer_system->buckets), i, smart_bucket_t);
        smart_chunk_t *sc = SMART_BUFF_GET(smart_buffer_system, bucket->min_size + 1);
        assert(sc);
        fprintf(stdout, "\t-> Successfully got a smart buffer of size %ld\n", bucket->min_size + 1);
        SMART_BUFF_RETURN(smart_buffer_system, bucket->min_size + 1, sc);
        assert(sc == NULL);
        fprintf(stdout, "\t\t-> now successfully returned\n");
    }

    fprintf(stdout, "\t-> ok\n");
    return 0;
}

int get_from_last_bucket(smart_buffers_t *smart_buffer_system)
{
    size_t queried_size;
    fprintf(stdout, "Getting an element in the last bucket\n");
    size_t *last_bucket_size = DYN_ARRAY_GET_ELT(&(smart_buffer_system->bucket_sizes), smart_buffer_system->num_buckets - 1, size_t);
    fprintf(stderr, "[DBG] l.%d\n", __LINE__);
    queried_size = *last_bucket_size + 1;
    fprintf(stderr, "[DBG] l.%d\n", __LINE__);
    smart_chunk_t *beyong_last_bucket_sc = SMART_BUFF_GET(smart_buffer_system, queried_size);
    fprintf(stderr, "[DBG] l.%d\n", __LINE__);
    if (beyong_last_bucket_sc == NULL)
    {
        return -1;
    }
    fprintf(stderr, "[DBG] l.%d\n", __LINE__);
    fprintf(stdout, "-> ok\n");
    return 0;
}

int empty_bucket(smart_buffers_t *smart_buffer_system, int bucket_id)
{
    size_t num_smart_chunks, i, queried_size;
    smart_bucket_t *sb;
    ucs_list_link_t chunks;
    ucs_list_head_init(&chunks);

    fprintf(stdout, "Getting all the chunks from bucket %d\n", bucket_id);
    sb = DYN_ARRAY_GET_ELT(&(smart_buffer_system->buckets), bucket_id, smart_bucket_t);
    num_smart_chunks = SIMPLE_LIST_LENGTH(&(sb->pool));
    queried_size = sb->max_size - 1;
    fprintf(stdout, "... %ld smart chunks to get\n", num_smart_chunks);

    for (i = 0; i < num_smart_chunks; i++)
    {
        smart_chunk_t *sc = SMART_BUFF_GET(smart_buffer_system, queried_size);
        assert(sc->in_use == true);
        ucs_list_add_tail(&chunks, &(sc->super));
    }
    DISPLAY_SMART_BUFFERS_SYS_DATA(smart_buffer_system);

    // Get one more which should get a chunk of bigger size to make more of this size available
    smart_chunk_t *sc = SMART_BUFF_GET(smart_buffer_system, queried_size);
    assert(sc);
    assert(sc->seq_num != UINT64_MAX);
    fprintf(stderr, "[DBG] chunk %" PRIu64 ", size %ld\n", sc->seq_num, queried_size);
    assert(sc->in_use == true);
    ucs_list_add_tail(&chunks, &(sc->super));
    DISPLAY_SMART_BUFFERS_SYS_DATA(smart_buffer_system);

    // Return all the elements
     while (!ucs_list_is_empty(&chunks))
    {
        smart_chunk_t *chunk = ucs_list_extract_head(&chunks, smart_chunk_t, super);
        assert(chunk);
        SMART_BUFF_RETURN(smart_buffer_system, queried_size, chunk);
        assert(chunk == NULL);
    }
    fprintf(stdout, "-> ok\n");

    return 0;
}

int main(int argc, char **argv)
{
    int rc;
    smart_buffers_t smart_buffer_system;

    // Initialize the smart buffer system with default configuration
    SMART_BUFFS_INIT(&smart_buffer_system, NULL);

    // Print smart buffer system's info
    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);

    rc = get_return_one_from_each_bucket(&smart_buffer_system);
    if (rc)
        goto error_out;
    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);

    rc = get_from_last_bucket(&smart_buffer_system);
    if (rc)
        goto error_out;
    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);

    rc = empty_bucket(&smart_buffer_system, 6);
    if (rc)
        goto error_out;

    DISPLAY_SMART_BUFFERS_SYS_DATA(&smart_buffer_system);

    // Finalize the smart buffer system
    fprintf(stdout, "Finalizing the smart buffer system\n");
    SMART_BUFFS_FINI(&smart_buffer_system);
    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
error_out:
    fprintf(stdout, "%s: test failed\n", argv[0]);
    return EXIT_FAILURE;
}
