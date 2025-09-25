#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <alloca.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define STACK_DEPTH 100

// Recursive function to grow stack
int recursive_stack_test(int depth, char pattern) {
    char large_buffer[4096];  // One page on stack
    int i;
    
    // Fill buffer with pattern
    memset(large_buffer, pattern, sizeof(large_buffer));
    
    // Verify pattern
    for (i = 0; i < sizeof(large_buffer); i++) {
        if (large_buffer[i] != pattern) {
            return -1;
        }
    }
    
    if (depth > 0) {
        // Recurse deeper
        return recursive_stack_test(depth - 1, pattern + 1);
    }
    
    // At maximum depth, verify we can still allocate
    char *dynamic_stack = alloca(1024);
    strcpy(dynamic_stack, "StackBottom");
    if (strcmp(dynamic_stack, "StackBottom") != 0) {
        return -2;
    }
    
    return 0;
}

int main(void) {
    long ret;
    struct rlimit rlim;
    char *stack_var;
    int result;
    
    printf("TEST13: Stack Growth Test\n");
    printf("=========================\n");
    
    // Get current stack limit
    if (getrlimit(RLIMIT_STACK, &rlim) != 0) {
        printf("FAIL: Cannot get stack limit: %s\n", strerror(errno));
        return 1;
    }
    printf("INFO: Current stack limit: soft=%lu, hard=%lu\n",
           (unsigned long)rlim.rlim_cur, (unsigned long)rlim.rlim_max);
    
    // Ensure reasonable stack size (at least 8MB)
    if (rlim.rlim_cur < 8 * 1024 * 1024) {
        rlim.rlim_cur = 8 * 1024 * 1024;
        if (setrlimit(RLIMIT_STACK, &rlim) != 0) {
            printf("WARN: Cannot increase stack limit: %s\n", strerror(errno));
        } else {
            printf("INFO: Increased stack limit to 8MB\n");
        }
    }
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Test 1: Simple stack allocation
    {
        char stack_buffer[8192];
        memset(stack_buffer, 'S', sizeof(stack_buffer));
        if (stack_buffer[0] != 'S' || stack_buffer[8191] != 'S') {
            printf("FAIL: Simple stack allocation failed\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        printf("PASS: Simple stack allocation works\n");
    }
    
    // Test 2: alloca() allocation
    stack_var = alloca(4096);
    if (!stack_var) {
        printf("FAIL: alloca() failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    strcpy(stack_var, "AllocaTest");
    if (strcmp(stack_var, "AllocaTest") != 0) {
        printf("FAIL: alloca() memory not working\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: alloca() works with replication\n");
    
    // Test 3: Deep recursion to grow stack
    printf("INFO: Starting deep recursion test (depth=%d)...\n", STACK_DEPTH);
    result = recursive_stack_test(STACK_DEPTH, 'A');
    if (result != 0) {
        printf("FAIL: Recursive stack test failed with code %d\n", result);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Deep recursion successful, stack grew correctly\n");
    
    // Test 4: Variable-length array (VLA)
    {
        int vla_size = 1024;
        char vla_buffer[vla_size];
        
        for (int i = 0; i < vla_size; i++) {
            vla_buffer[i] = (char)(i % 256);
        }
        
        for (int i = 0; i < vla_size; i++) {
            if (vla_buffer[i] != (char)(i % 256)) {
                printf("FAIL: VLA verification failed at index %d\n", i);
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                return 1;
            }
        }
        printf("PASS: Variable-length array works\n");
    }
    
    // Test 5: Large alloca to trigger multiple page allocations
    size_t large_size = 4096 * 5;  // 5 pages
    stack_var = alloca(large_size);
    if (!stack_var) {
        printf("WARN: Large alloca failed (may be stack limit)\n");
    } else {
        memset(stack_var, 'L', large_size);
        if (stack_var[0] != 'L' || stack_var[large_size - 1] != 'L') {
            printf("FAIL: Large alloca memory not working\n");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        printf("PASS: Large alloca (5 pages) works\n");
    }
    
    // Test 6: Stack pointer alignment check
    void *sp1 = alloca(1);
    void *sp2 = alloca(1);
    ptrdiff_t diff = (char *)sp1 - (char *)sp2;
    printf("INFO: Stack growth direction: %s (%ld bytes)\n",
           diff > 0 ? "downward" : "upward", (long)diff);
    
    // Verify replication still enabled after stack operations
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Replication disabled after stack operations\n");
        return 1;
    }
    printf("PASS: Replication still enabled (0x%lx)\n", ret);
    
    // Clean up
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST13: SUCCESS - Stack growth works with replication\n");
    return 0;
}
