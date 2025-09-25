// test31.c - Process resource limits and replication test
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    int ret;
    struct rlimit rlim, orig_rlim;
    void *test_mem;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    if (numa_num_configured_nodes() < 2) {
        printf("SKIP: Need at least 2 NUMA nodes\n");
        return 0;
    }
    
    // Test 1: Enable replication with default limits
    printf("Testing with default limits...\n");
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication with default limits: %s\n", strerror(errno));
        return 1;
    }
    
    unsigned long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication not enabled\n");
        return 1;
    }
    
    // Disable for next test
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    // Test 2: Very low data segment limit
    printf("Testing with low data segment limit...\n");
    if (getrlimit(RLIMIT_DATA, &orig_rlim) < 0) {
        printf("WARNING: Could not get RLIMIT_DATA\n");
    } else {
        rlim.rlim_cur = 16 * 1024 * 1024;  // 16MB
        rlim.rlim_max = orig_rlim.rlim_max;
        
        if (setrlimit(RLIMIT_DATA, &rlim) < 0) {
            printf("WARNING: Could not set RLIMIT_DATA: %s\n", strerror(errno));
        } else {
            ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
            if (ret < 0) {
                printf("INFO: Replication rejected with low data limit (expected): %s\n", 
                       strerror(errno));
            } else {
                // It worked - verify we can still allocate
                test_mem = malloc(1024 * 1024);
                if (!test_mem) {
                    printf("FAIL: Cannot allocate with low limit\n");
                    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                    setrlimit(RLIMIT_DATA, &orig_rlim);
                    return 1;
                }
                memset(test_mem, 0xAA, 1024 * 1024);
                if (((char*)test_mem)[0] != (char)0xAA) {
                    printf("FAIL: Memory verification failed\n");
                    free(test_mem);
                    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                    setrlimit(RLIMIT_DATA, &orig_rlim);
                    return 1;
                }
                free(test_mem);
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            }
            
            // Restore limit
            setrlimit(RLIMIT_DATA, &orig_rlim);
        }
    }
    
    // Test 3: Stack size limit
    printf("Testing with modified stack size limit...\n");
    if (getrlimit(RLIMIT_STACK, &orig_rlim) < 0) {
        printf("WARNING: Could not get RLIMIT_STACK\n");
    } else {
        rlim.rlim_cur = 2 * 1024 * 1024;  // 2MB
        rlim.rlim_max = orig_rlim.rlim_max;
        
        if (setrlimit(RLIMIT_STACK, &rlim) < 0) {
            printf("WARNING: Could not set RLIMIT_STACK: %s\n", strerror(errno));
        } else {
            ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
            if (ret < 0) {
                printf("INFO: Replication failed with stack limit: %s\n", strerror(errno));
            } else {
                // Test stack allocation
                char stack_test[1024];
                memset(stack_test, 0xBB, sizeof(stack_test));
                if (stack_test[0] != (char)0xBB || stack_test[1023] != (char)0xBB) {
                    printf("FAIL: Stack memory test failed\n");
                    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                    setrlimit(RLIMIT_STACK, &orig_rlim);
                    return 1;
                }
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            }
            
            // Restore limit
            setrlimit(RLIMIT_STACK, &orig_rlim);
        }
    }
    
    // Test 4: Number of processes limit (should not affect single process)
    printf("Testing with process number limit...\n");
    if (getrlimit(RLIMIT_NPROC, &orig_rlim) < 0) {
        printf("WARNING: Could not get RLIMIT_NPROC\n");
    } else {
        rlim.rlim_cur = 10;  // Very low process limit
        rlim.rlim_max = orig_rlim.rlim_max;
        
        if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
            printf("WARNING: Could not set RLIMIT_NPROC: %s\n", strerror(errno));
        } else {
            ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
            if (ret < 0) {
                printf("FAIL: Process limit should not affect replication: %s\n", 
                       strerror(errno));
                setrlimit(RLIMIT_NPROC, &orig_rlim);
                return 1;
            }
            
            status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
            if (status == 0) {
                printf("FAIL: Replication not enabled with NPROC limit\n");
                setrlimit(RLIMIT_NPROC, &orig_rlim);
                return 1;
            }
            
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            setrlimit(RLIMIT_NPROC, &orig_rlim);
        }
    }
    
    // Test 5: CPU time limit (should not affect)
    printf("Testing with CPU time limit...\n");
    if (getrlimit(RLIMIT_CPU, &orig_rlim) < 0) {
        printf("WARNING: Could not get RLIMIT_CPU\n");
    } else {
        rlim.rlim_cur = 60;  // 60 seconds
        rlim.rlim_max = orig_rlim.rlim_max;
        
        if (setrlimit(RLIMIT_CPU, &rlim) < 0) {
            printf("WARNING: Could not set RLIMIT_CPU: %s\n", strerror(errno));
        } else {
            ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
            if (ret < 0) {
                printf("FAIL: CPU limit should not affect replication: %s\n", 
                       strerror(errno));
                setrlimit(RLIMIT_CPU, &orig_rlim);
                return 1;
            }
            
            status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
            if (status == 0) {
                printf("FAIL: Replication not enabled with CPU limit\n");
                setrlimit(RLIMIT_CPU, &orig_rlim);
                return 1;
            }
            
            // Do some work to verify it functions
            test_mem = malloc(4096);
            if (!test_mem) {
                printf("FAIL: Allocation failed with CPU limit\n");
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                setrlimit(RLIMIT_CPU, &orig_rlim);
                return 1;
            }
            
            for (int i = 0; i < 1000; i++) {
                memset(test_mem, i & 0xFF, 4096);
            }
            free(test_mem);
            
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            setrlimit(RLIMIT_CPU, &orig_rlim);
        }
    }
    
    printf("PASS: Process resource limits test completed successfully\n");
    return 0;
}
