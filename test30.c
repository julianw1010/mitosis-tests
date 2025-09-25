// test30.c - NUMA memory policy with replication test
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <string.h>
#include <errno.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    int ret;
    void *test_mem;
    size_t alloc_size = 8 * 1024 * 1024; // 8MB
    int num_nodes;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    num_nodes = numa_num_configured_nodes();
    if (num_nodes < 2) {
        printf("SKIP: Need at least 2 NUMA nodes\n");
        return 0;
    }
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    // Verify enabled
    unsigned long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication not enabled\n");
        return 1;
    }
    
    // Test 1: Default policy allocation
    printf("Testing default policy allocation...\n");
    test_mem = malloc(alloc_size);
    if (!test_mem) {
        printf("FAIL: Initial allocation failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    memset(test_mem, 0x11, alloc_size);
    if (((char*)test_mem)[0] != 0x11 || ((char*)test_mem)[alloc_size-1] != 0x11) {
        printf("FAIL: Default policy memory verification failed\n");
        free(test_mem);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    free(test_mem);
    
    // Test 2: Bind to specific node
    printf("Testing bind to node 0...\n");
    unsigned long nodemask = 1UL << 0;
    ret = set_mempolicy(MPOL_BIND, &nodemask, num_nodes + 1);
    if (ret < 0) {
        printf("WARNING: Could not set MPOL_BIND policy: %s\n", strerror(errno));
    } else {
        test_mem = malloc(alloc_size);
        if (!test_mem) {
            printf("FAIL: Allocation with MPOL_BIND failed\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        
        memset(test_mem, 0x22, alloc_size);
        if (((char*)test_mem)[0] != 0x22 || ((char*)test_mem)[alloc_size-1] != 0x22) {
            printf("FAIL: MPOL_BIND memory verification failed\n");
            free(test_mem);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        free(test_mem);
    }
    
    // Test 3: Interleave policy
    printf("Testing interleave policy...\n");
    unsigned long all_nodes = (1UL << num_nodes) - 1;
    ret = set_mempolicy(MPOL_INTERLEAVE, &all_nodes, num_nodes + 1);
    if (ret < 0) {
        printf("WARNING: Could not set MPOL_INTERLEAVE policy: %s\n", strerror(errno));
    } else {
        test_mem = malloc(alloc_size);
        if (!test_mem) {
            printf("FAIL: Allocation with MPOL_INTERLEAVE failed\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        
        memset(test_mem, 0x33, alloc_size);
        if (((char*)test_mem)[0] != 0x33 || ((char*)test_mem)[alloc_size-1] != 0x33) {
            printf("FAIL: MPOL_INTERLEAVE memory verification failed\n");
            free(test_mem);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        free(test_mem);
    }
    
    // Test 4: Preferred node policy
    printf("Testing preferred node policy...\n");
    ret = set_mempolicy(MPOL_PREFERRED, &nodemask, num_nodes + 1);
    if (ret < 0) {
        printf("WARNING: Could not set MPOL_PREFERRED policy: %s\n", strerror(errno));
    } else {
        test_mem = malloc(alloc_size);
        if (!test_mem) {
            printf("FAIL: Allocation with MPOL_PREFERRED failed\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        
        memset(test_mem, 0x44, alloc_size);
        if (((char*)test_mem)[0] != 0x44 || ((char*)test_mem)[alloc_size-1] != 0x44) {
            printf("FAIL: MPOL_PREFERRED memory verification failed\n");
            free(test_mem);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        free(test_mem);
    }
    
    // Reset to default policy
    set_mempolicy(MPOL_DEFAULT, NULL, 0);
    
    // Test 5: mbind on existing memory
    printf("Testing mbind on existing memory...\n");
    test_mem = malloc(alloc_size);
    if (!test_mem) {
        printf("FAIL: Allocation for mbind test failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    memset(test_mem, 0x55, alloc_size);
    
    // Try to bind existing memory to node 0
    ret = mbind(test_mem, alloc_size, MPOL_BIND, &nodemask,
                num_nodes + 1, MPOL_MF_MOVE);
    if (ret < 0 && errno != ENOSYS && errno != EPERM) {
        printf("WARNING: mbind failed: %s\n", strerror(errno));
    }
    
    // Verify memory still accessible
    if (((char*)test_mem)[0] != 0x55 || ((char*)test_mem)[alloc_size-1] != 0x55) {
        printf("FAIL: Memory corrupted after mbind\n");
        free(test_mem);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    free(test_mem);
    
    // Verify replication still enabled
    status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication disabled during NUMA policy changes\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("WARNING: Could not disable replication\n");
    }
    
    printf("PASS: NUMA memory policy test completed successfully\n");
    return 0;
}
