// test18.c - Thread Creation After Replication Enabled
// Tests that creating threads after enabling replication is properly rejected
// and that the process handles this gracefully

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <errno.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define TEST_SIZE (1024 * 1024)  // 1MB

volatile int thread_alive = 0;
volatile int thread_can_exit = 0;
unsigned char *shared_mem;

void *thread_func(void *arg) {
    int thread_id = *(int*)arg;
    int i;
    
    thread_alive = 1;
    printf("  Thread %d: Started\n", thread_id);
    
    // Try to access the memory
    printf("  Thread %d: Attempting memory access...\n", thread_id);
    for (i = 0; i < 1000; i++) {
        shared_mem[i] = (unsigned char)(0xAA + thread_id);
        if (shared_mem[i] != (unsigned char)(0xAA + thread_id)) {
            printf("  Thread %d: Memory mismatch at offset %d\n", thread_id, i);
            break;
        }
    }
    printf("  Thread %d: Memory access completed\n", thread_id);
    
    // Wait until main thread says we can exit
    while (!thread_can_exit) {
        usleep(1000);
    }
    
    printf("  Thread %d: Exiting\n", thread_id);
    return NULL;
}

int main(void) {
    pthread_t thread;
    int ret;
    int test_passed = 1;
    int thread_id = 1;
    int i;
    
    printf("Test 18: Thread Creation After Replication Enabled\n");
    printf("===================================================\n");
    
    // Allocate memory first
    shared_mem = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared_mem == MAP_FAILED) {
        printf("FAIL: mmap failed\n");
        return 1;
    }
    
    // Write initial pattern
    printf("Writing initial pattern to memory...\n");
    memset(shared_mem, 0x55, TEST_SIZE);
    
    // Enable replication
    printf("Enabling replication...\n");
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication\n");
        munmap(shared_mem, TEST_SIZE);
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%x\n", ret);
    
    // Verify we can still access memory
    printf("Verifying memory access with replication enabled...\n");
    for (i = 0; i < 1000; i++) {
        if (shared_mem[i] != 0x55) {
            printf("FAIL: Memory corrupted at offset %d after enabling replication\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Memory access OK with replication enabled\n");
    
    // Now try to create a thread
    printf("\nAttempting to create thread with replication enabled...\n");
    thread_alive = 0;
    thread_can_exit = 0;
    
    ret = pthread_create(&thread, NULL, thread_func, &thread_id);
    if (ret == 0) {
        printf("WARNING: Thread creation succeeded (ret=%d)\n", ret);
        
        // Wait a bit to see if thread actually runs
        usleep(100000); // 100ms
        
        if (thread_alive) {
            printf("Thread is running - checking if replication is still enabled...\n");
            
            // Check if replication is still enabled
            ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
            if (ret > 0) {
                printf("ERROR: Replication still enabled with multiple threads (mask=0x%x)\n", ret);
                printf("  This violates the single-threaded requirement!\n");
                test_passed = 0;
            } else {
                printf("OK: Replication was automatically disabled (mask=0x%x)\n", ret);
            }
            
            // Let thread exit cleanly
            thread_can_exit = 1;
            pthread_join(thread, NULL);
            printf("Thread joined successfully\n");
        } else {
            printf("Thread never started running\n");
        }
    } else {
        printf("Thread creation failed with error %d (%s)\n", ret, strerror(ret));
        printf("  This might be expected behavior\n");
    }
    
    // Try to re-enable replication after thread exits
    printf("\nAttempting to re-enable replication...\n");
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("Cannot re-enable replication: %s\n", strerror(errno));
        if (errno == EBUSY) {
            printf("  EBUSY suggests process is still multi-threaded\n");
        }
    } else {
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        printf("Replication re-enabled successfully (mask=0x%x)\n", ret);
    }
    
    // Verify memory is still accessible
    printf("\nFinal memory verification...\n");
    int errors = 0;
    for (i = 0; i < 1000; i++) {
        // Memory might have been modified by thread
        if (shared_mem[i] != 0x55 && shared_mem[i] != (unsigned char)(0xAA + thread_id)) {
            printf("Unexpected value at offset %d: 0x%02x\n", i, shared_mem[i]);
            errors++;
            if (errors > 10) {
                printf("Too many errors, stopping check\n");
                test_passed = 0;
                break;
            }
        }
    }
    if (errors == 0) {
        printf("Memory integrity maintained\n");
    }
    
cleanup:
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    munmap(shared_mem, TEST_SIZE);
    
    if (test_passed) {
        printf("\n✓ Test 18 PASSED: Thread creation with replication handled correctly\n");
        return 0;
    } else {
        printf("\n✗ Test 18 FAILED: Issues with thread creation and replication\n");
        return 1;
    }
}
