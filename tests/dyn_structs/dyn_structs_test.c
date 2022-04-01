//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdlib.h>
#include <stdio.h>

#include "dynamic_structs.h"

#define NUM_ELEMENTS (1000000)

typedef struct dummy
{
    ucs_list_link_t item;
    char elt[NUM_ELEMENTS];
} dummy_t;

int main(int argc, char **argv)
{
    dyn_list_t *list_1 = NULL;
    DYN_LIST_ALLOC(list_1, 1, dummy_t, item);

    if (list_1->num_mem_chunks != 1)
    {
        fprintf(stderr, "Number of mem chunks is %ld instead of expected 1\n", list_1->num_mem_chunks);
        goto error_out;
    }
    if (list_1->mem_chunks[0].size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", list_1->mem_chunks[0].size, sizeof(dummy_t));
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
    if (list_1->mem_chunks[0].size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", list_1->mem_chunks[0].size, sizeof(dummy_t));
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
    if (list_1->mem_chunks[0].size != sizeof(dummy_t))
    {
        fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", list_1->mem_chunks[0].size, sizeof(dummy_t));
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
            if (list_1->mem_chunks[j].size != sizeof(dummy_t))
            {
                fprintf(stderr, "Mem chunk 0 is of size %ld instead of expected %ld\n", list_1->mem_chunks[j].size, sizeof(dummy_t));
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
            if (list_1->mem_chunks[j].size != sizeof(dummy_t))
            {
                fprintf(stderr, "l.%d: Mem chunk %ld is of size %ld instead of expected %ld\n",
                        __LINE__,
                        j,
                        list_1->mem_chunks[j].size,
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
    fprintf(stderr, "\nTEST SUCCESSFULLY COMPLETED\n");
    return EXIT_SUCCESS;

error_out:
    if (list_1 != NULL)
        DYN_LIST_FREE(list_1, dummy_t, item);
    free(array_elements);
    fprintf(stderr, "\nTEST FAILED\n");
    return EXIT_FAILURE;
}