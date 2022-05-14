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

#define NUM_ELEMENTS (1000000)

typedef struct dummy
{
    ucs_list_link_t item;
    char elt[NUM_ELEMENTS];
} dummy_t;

bool check_array(dyn_array_t *a, uint64_t expected_size)
{
    if (a->base == NULL)
    {
        fprintf(stderr, "The array's base is undefined\n");
        return false;
    }

    if (a->num_elts != expected_size)
    {
        fprintf(stderr, "Number of elements is %" PRIu64 " instead of the expected %" PRIu64 "\n",
                a->num_elts, expected_size);
        return false;
    }

    return true;
}

int test_dyn_array(int argc, char **argv)
{
    // Create a small array and request an element way behind the end
    // to see if the array was correctly grown
    dyn_array_t array;
    DYN_ARRAY_ALLOC(&array, 8, int);
    check_array(&array, 8);

    int *dummy = DYN_ARRAY_GET_ELT(&array, 1024, int);
    if (dummy == NULL)
    {
        fprintf(stderr, "unable to get element 1024 from the array\n");
        goto error_out;
    }
    check_array(&array, 1032);

    fprintf(stderr, "\nDYN_ARRAY TEST SUCCESSFULLY COMPLETED\n\n");
    return EXIT_SUCCESS;
error_out:
    fprintf(stderr, "\nDYN_ARRAY TEST FAILED\n\n");
    return EXIT_FAILURE;
}

int test_dyn_list(int argc, char **argv)
{
    dyn_list_t *list_1 = NULL;
    DYN_LIST_ALLOC(list_1, 1, dummy_t, item);

    if (list_1->num_mem_chunks != 1)
    {
        fprintf(stderr, "Number of mem chunks is %ld instead of expected 1\n", list_1->num_mem_chunks);
        goto error_out;
    }
    // First we check the chunks of memory, which are stored in a dynamic array
    mem_chunk_t *mem_chunk_ptr = DYN_ARRAY_GET_ELT(&(list_1->mem_chunks), 0UL, mem_chunk_t);
    if (mem_chunk_ptr == NULL)
    {
        fprintf(stderr, "Unable to get mem chunk #0\n");
        goto error_out;
    }
    if (mem_chunk_ptr->size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", mem_chunk_ptr->size, sizeof(dummy_t));
        goto error_out;
    }
    if (list_1->num_elts_alloc != 1)
    {
        fprintf(stderr, "Number of elements for allocations is %ld instead of 1\n", list_1->num_elts_alloc);
        goto error_out;
    }
    if (list_1->num_elts != 1)
    {
        fprintf(stderr, "Number of elements is %ld instead of 1\n", list_1->num_elts);
        goto error_out;
    }
    if (ucs_list_length(&(list_1->list)) != 1)
    {
        fprintf(stderr, "Number of elements in list is %ld instead of 1\n", ucs_list_length(&(list_1->list)));
        goto error_out;
    }

    /* Get one element and check everything is fine */
    dummy_t **array_elements = malloc(1000 * sizeof(dummy_t));
    dummy_t *ptr = array_elements[0];
    DYN_LIST_GET(list_1, dummy_t, item, ptr);
    if (ptr == NULL)
    {
        fprintf(stderr, "Unable to get element from list\n");
        goto error_out;
    }
    fprintf(stderr, "Elt: %p\n", ptr);
    if (list_1->num_mem_chunks != 1)
    {
        fprintf(stderr, "Number of mem chunks is %ld instead of expected 1\n", list_1->num_mem_chunks);
        goto error_out;
    }
    mem_chunk_ptr = DYN_ARRAY_GET_ELT(&(list_1->mem_chunks), 0UL, mem_chunk_t);
    if (mem_chunk_ptr == NULL)
    {
        fprintf(stderr, "Unable to get mem chunk #0\n");
        goto error_out;
    }
    if (mem_chunk_ptr->size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", mem_chunk_ptr->size, sizeof(dummy_t));
        goto error_out;
    }
    if (list_1->num_elts_alloc != 1)
    {
        fprintf(stderr, "Number of elements for allocations is %ld instead of 1\n", list_1->num_elts_alloc);
        goto error_out;
    }
    if (!ucs_list_is_empty(&(list_1->list)))
    {
        fprintf(stderr, "Number of elements in list is %ld instead of 0\n", ucs_list_length(&(list_1->list)));
        goto error_out;
    }

    // Return the event and check everything is fine
    DYN_LIST_RETURN(list_1, ptr, item);
    if (list_1->num_mem_chunks != 1)
    {
        fprintf(stderr, "Number of mem chunks is %ld instead of expected 1\n", list_1->num_mem_chunks);
        goto error_out;
    }
    mem_chunk_ptr = DYN_ARRAY_GET_ELT(&(list_1->mem_chunks), 0UL, mem_chunk_t);
    if (mem_chunk_ptr == NULL)
    {
        fprintf(stderr, "Unable to get mem chunk #0\n");
        goto error_out;
    }
    if (mem_chunk_ptr->size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", mem_chunk_ptr->size, sizeof(dummy_t));
        goto error_out;
    }
    if (list_1->num_elts_alloc != 1)
    {
        fprintf(stderr, "Number of elements for allocations is %ld instead of 1\n", list_1->num_elts_alloc);
        goto error_out;
    }
    if (ucs_list_length(&(list_1->list)) != 1)
    {
        fprintf(stderr, "Number of elements in list is %ld instead of 0\n", ucs_list_length(&(list_1->list)));
        goto error_out;
    }

    fprintf(stderr, "-> test with a single element succeeded\n");

    // Then try to get 1000 elements
    size_t i;
    for (i = 0; i < 1000; i++)
    {
        fprintf(stderr, "Getting element %ld\n", i);
        DYN_LIST_GET(list_1, dummy_t, item, (array_elements[i]));
        if (array_elements[i] == NULL)
        {
            fprintf(stderr, "Unable to get element from list\n");
            goto error_out;
        }
        if (list_1->num_mem_chunks != (i + 1))
        {
            fprintf(stderr, "Number of mem chunks is %ld instead of expected %ld\n", list_1->num_mem_chunks, i + 1);
            goto error_out;
        }
        size_t j;
        for (j = 0; j <= i; j++)
        {
            mem_chunk_ptr = DYN_ARRAY_GET_ELT(&(list_1->mem_chunks), j, mem_chunk_t);
            if (mem_chunk_ptr == NULL)
            {
                fprintf(stderr, "Unable to get mem chunk #%ld\n", j);
                goto error_out;
            }
            if (mem_chunk_ptr->size != sizeof(dummy_t))
            {
                fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", mem_chunk_ptr->size, sizeof(dummy_t));
                goto error_out;
            }
        }
        if (list_1->num_elts_alloc != 1)
        {
            fprintf(stderr, "Number of elements for allocations is %ld instead of 1\n", list_1->num_elts_alloc);
            goto error_out;
        }
        if (!ucs_list_is_empty(&(list_1->list)))
        {
            fprintf(stderr, "Number of elements in list is %ld instead of 0\n", ucs_list_length(&(list_1->list)));
            goto error_out;
        }
    }
    fprintf(stderr, "Successfully got the 1000 elements\n");
    for (i = 0; i < 1000; i++)
    {
        fprintf(stderr, "Returning element %ld (%p)\n", i, array_elements[i]);
        DYN_LIST_RETURN(list_1, (array_elements[i]), item);
        if (ptr == NULL)
        {
            fprintf(stderr, "Unable to get element from list\n");
            goto error_out;
        }
        fprintf(stderr, "-> checking the status\n");
        if (list_1->num_mem_chunks != 1000)
        {
            fprintf(stderr, "Number of mem chunks is %ld instead of expected 1000\n", list_1->num_mem_chunks);
            goto error_out;
        }
        size_t j;
        for (j = 0; j < 1000; j++)
        {
            mem_chunk_ptr = DYN_ARRAY_GET_ELT(&(list_1->mem_chunks), j, mem_chunk_t);
            if (mem_chunk_ptr == NULL)
            {
                fprintf(stderr, "Unable to get mem chunk #%ld\n", j);
                goto error_out;
            }
            if (mem_chunk_ptr->size != sizeof(dummy_t))
            {
                fprintf(stderr, "l.%d: Mem chunk %ld is of size %ld instead of expected %ld\n",
                        __LINE__,
                        j,
                        mem_chunk_ptr->size,
                        sizeof(dummy_t));
                goto error_out;
            }
        }
        if (list_1->num_elts_alloc != 1)
        {
            fprintf(stderr, "Number of elements for allocations is %ld instead of 1\n", list_1->num_elts_alloc);
            goto error_out;
        }
        if (ucs_list_length(&(list_1->list)) != i + 1)
        {
            fprintf(stderr, "Number of elements in list is %ld instead of %ld\n", ucs_list_length(&(list_1->list)), i + 1);
            goto error_out;
        }
    }

    /* Clean up / termination */
    DYN_LIST_FREE(list_1, dummy_t, item);
    free(array_elements);
    fprintf(stderr, "\nDYN_LIST TEST SUCCESSFULLY COMPLETED\n\n");
    return EXIT_SUCCESS;

error_out:
    if (list_1 != NULL)
        DYN_LIST_FREE(list_1, dummy_t, item);
    free(array_elements);
    fprintf(stderr, "\nDYN_LIST TEST FAILED\n\n");
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    int rc = test_dyn_list(argc, argv);
    if (rc == EXIT_FAILURE)
    {
        fprintf(stderr, "test_dyn_list() failed\n");
        return EXIT_FAILURE;
    }

    rc = test_dyn_array(argc, argv);
    if (rc == EXIT_FAILURE)
    {
        fprintf(stderr, "test_dyn_array() failed\n");
        return EXIT_FAILURE;
    }

    fprintf(stdout, "%s: test succeeded\n", argv[0]);
    return EXIT_SUCCESS;
}