//
// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
//
// See LICENSE.txt for license information
//

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <dpu_offload_types.h>

int main(int argc, char **argv)
{
    char *host1, *host2, *host3, *hostA, *hostB, *hostC;
    uint64_t hash_host1, hash_host2, hash_host3, hash_hostA, hash_hostB, hash_hostC;
    host1 = "myfancyhostnamepatternthatiamtheonlyonetouse00001.whateverdomainthatiamalsotheonlyonetouse.com\0";
    host2 = "myfancyhostnamepatternthatiamtheonlyonetouse00002.whateverdomainthatiamalsotheonlyonetouse.com\0";
    host3 = "myfancyhostnamepatternthatiamtheonlyonetouse00003.whateverdomainthatiamalsotheonlyonetouse.com\0";
    hostA = "1.d.net\0";
    hostB = "2.d.net\0";
    hostC = "3.d.net\0";

    hash_host1 = HASH64_FROM_STRING(host1, strlen(host1));
    fprintf(stdout, "Hash for %s is 0x%lx\n", host1, hash_host1);
    hash_host2 = HASH64_FROM_STRING(host2, strlen(host2));
    fprintf(stdout, "Hash for %s is 0x%lx\n", host2, hash_host2);
    hash_host3 = HASH64_FROM_STRING(host3, strlen(host3));
    fprintf(stdout, "Hash for %s is 0x%lx\n", host3, hash_host3);
    hash_hostA = HASH64_FROM_STRING(hostA, strlen(hostA));
    fprintf(stdout, "Hash for %s is 0x%lx\n", hostA, hash_hostA);
    hash_hostB = HASH64_FROM_STRING(hostB, strlen(hostB));
    fprintf(stdout, "Hash for %s is 0x%lx\n", hostB, hash_hostB);
    hash_hostC = HASH64_FROM_STRING(hostC, strlen(hostC));
    fprintf(stdout, "Hash for %s is 0x%lx\n", hostC, hash_hostC);

    if (hash_host1 == hash_host2 || hash_host1 == hash_host3 || hash_host2 == hash_host3)
    {
        fprintf(stderr, "ERROR: hash collision detected (l.%d)\n", __LINE__);
        return EXIT_FAILURE;
    }

    if (hash_hostA == hash_hostB || hash_hostA == hash_hostC || hash_hostB == hash_hostC)
    {
        fprintf(stderr, "ERROR: hash collision detected (l.%d)\n", __LINE__);
        return EXIT_FAILURE;
    }

    fprintf(stdout, "\n%s successfully terminated\n", argv[0]);
    return EXIT_SUCCESS;
}