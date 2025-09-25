#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    
    printf("TEST1: Basic Enable/Disable Mitosis Replication\n");
    printf("================================================\n");
    
    // Check initial state - should be disabled (0)
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: PR_GET_PGTABLE_REPL failed: %s\n", strerror(errno));
        return 1;
    }
    if (ret != 0) {
        printf("FAIL: Initial state should be 0 (disabled), but got %ld\n", ret);
        return 1;
    }
    printf("PASS: Initial state is disabled (0)\n");
    
    // Enable replication on all nodes
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: PR_SET_PGTABLE_REPL enable failed: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Enabled replication\n");
    
    // Verify it's enabled - should return non-zero bitmask
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: PR_GET_PGTABLE_REPL after enable failed: %s\n", strerror(errno));
        return 1;
    }
    if (ret == 0) {
        printf("FAIL: Should be enabled but got 0\n");
        return 1;
    }
    printf("PASS: Replication is enabled (bitmask=0x%lx)\n", ret);
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: PR_SET_PGTABLE_REPL disable failed: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Disabled replication\n");
    
    // Verify it's disabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: PR_GET_PGTABLE_REPL after disable failed: %s\n", strerror(errno));
        return 1;
    }
    if (ret != 0) {
        printf("FAIL: Should be disabled but got %ld\n", ret);
        return 1;
    }
    printf("PASS: Replication is disabled (0)\n");
    
    printf("\nTEST1: SUCCESS - All checks passed\n");
    return 0;
}
