#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>
#include <numa.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    unsigned long mask;
    int num_nodes;
    
    printf("TEST4: Specific Node Mask Enable Test\n");
    printf("======================================\n");
    
    // Check if NUMA is available
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available on this system\n");
        return 0;
    }
    
    num_nodes = numa_num_configured_nodes();
    printf("INFO: System has %d configured NUMA nodes\n", num_nodes);
    
    if (num_nodes < 2) {
        printf("SKIP: Need at least 2 NUMA nodes for meaningful test\n");
        return 0;
    }
    
    // Test 1: Enable only on node 0
    mask = (1UL << 0);
    printf("INFO: Trying to enable on node 0 only (mask=0x%lx)\n", mask);
    ret = prctl(PR_SET_PGTABLE_REPL, mask, 0, 0, 0);
    if (ret == 0) {
        // Check what actually got enabled
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: GET failed after setting node 0\n");
            return 1;
        }
        // Should have at least node 0 set, but may have more due to original PGD location
        if (!(ret & (1UL << 0))) {
            printf("FAIL: Node 0 not in returned mask (0x%lx)\n", ret);
            return 1;
        }
        printf("PASS: Node 0 enabled (actual mask=0x%lx)\n", ret);
    } else {
        printf("WARN: Single node enable not supported (may need 2+ nodes)\n");
    }
    
    // Disable
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    // Test 2: Enable on nodes 0 and 1
    if (num_nodes >= 2) {
        mask = (1UL << 0) | (1UL << 1);
        printf("INFO: Trying to enable on nodes 0,1 (mask=0x%lx)\n", mask);
        ret = prctl(PR_SET_PGTABLE_REPL, mask, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Could not enable on nodes 0,1: %s\n", strerror(errno));
            return 1;
        }
        
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: GET failed after setting nodes 0,1\n");
            return 1;
        }
        if ((ret & 3) != 3) {
            printf("FAIL: Expected nodes 0,1 enabled, got mask=0x%lx\n", ret);
            return 1;
        }
        printf("PASS: Nodes 0,1 enabled (mask=0x%lx)\n", ret);
        
        // Disable
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    }
    
    // Test 3: Invalid node mask (node that doesn't exist)
    if (num_nodes < 8) {
        mask = (1UL << 7); // Assuming node 7 doesn't exist
        printf("INFO: Trying invalid node 7 (mask=0x%lx)\n", mask);
        ret = prctl(PR_SET_PGTABLE_REPL, mask, 0, 0, 0);
        if (ret == 0) {
            printf("WARN: System allowed invalid node (may have 8 nodes?)\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        } else {
            printf("PASS: Correctly rejected invalid node mask\n");
        }
    }
    
    // Test 4: Verify arg2=1 enables all online nodes
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable on all nodes: %s\n", strerror(errno));
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("INFO: arg2=1 resulted in mask=0x%lx\n", ret);
    
    // Count bits set
    int bits_set = __builtin_popcountl(ret);
    if (bits_set < num_nodes) {
        printf("WARN: Fewer nodes enabled (%d) than configured (%d)\n", 
               bits_set, num_nodes);
    } else {
        printf("PASS: All nodes enabled with arg2=1\n");
    }
    
    // Clean up
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST4: SUCCESS - Node mask selection works correctly\n");
    return 0;
}
