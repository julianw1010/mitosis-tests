// test26.c - Clone syscall with different flags test
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

#define STACK_SIZE (1024 * 1024)

static int child_func(void *arg) {
    unsigned long repl_status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    
    // Child should NOT have replication enabled
    if (repl_status != 0) {
        printf("FAIL: Child has replication status 0x%lx (expected 0)\n", repl_status);
        return 1;
    }
    
    // Verify child can access memory
    int *test_mem = malloc(4096);
    if (!test_mem) {
        printf("FAIL: Child malloc failed\n");
        return 1;
    }
    *test_mem = 42;
    if (*test_mem != 42) {
        printf("FAIL: Child memory access incorrect\n");
        free(test_mem);
        return 1;
    }
    free(test_mem);
    
    return 0;
}

int main(void) {
    int ret;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    if (numa_num_configured_nodes() < 2) {
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
        printf("FAIL: Replication not enabled after prctl\n");
        return 1;
    }
    
    // Allocate stack for clone
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        printf("FAIL: Stack allocation failed\n");
        return 1;
    }
    char *stack_top = stack + STACK_SIZE;
    
    // Test 1: Clone with CLONE_VM (should fail or disable replication)
    pid_t pid = clone(child_func, stack_top, CLONE_VM | SIGCHLD, NULL);
    if (pid > 0) {
        // Parent - replication should be disabled
        status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (status != 0) {
            printf("FAIL: Parent still has replication after CLONE_VM\n");
            free(stack);
            return 1;
        }
        
        int wstatus;
        waitpid(pid, &wstatus, 0);
        
        // Re-enable for next test
        ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Could not re-enable replication\n");
            free(stack);
            return 1;
        }
    } else if (pid < 0 && errno == EINVAL) {
        // Clone might reject CLONE_VM with replication - that's OK
        printf("INFO: CLONE_VM rejected with replication (expected)\n");
    } else if (pid < 0) {
        printf("FAIL: Clone failed: %s\n", strerror(errno));
        free(stack);
        return 1;
    }
    
    // Test 2: Clone without CLONE_VM (normal fork-like behavior)
    pid = clone(child_func, stack_top, SIGCHLD, NULL);
    if (pid < 0) {
        printf("FAIL: Clone without CLONE_VM failed: %s\n", strerror(errno));
        free(stack);
        return 1;
    }
    
    if (pid == 0) {
        // We're in child - this shouldn't happen with clone() and separate stack
        _exit(child_func(NULL));
    }
    
    // Parent - should still have replication
    status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Parent lost replication after clone without CLONE_VM\n");
        free(stack);
        return 1;
    }
    
    int wstatus;
    waitpid(pid, &wstatus, 0);
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
        printf("FAIL: Child returned error\n");
        free(stack);
        return 1;
    }
    
    // Cleanup
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("WARNING: Could not disable replication\n");
    }
    
    free(stack);
    printf("PASS: Clone syscall test completed successfully\n");
    return 0;
}
