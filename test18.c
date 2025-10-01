// test18.c - Thread Creation With Replication Enabled
// Tests that threads can be created while replication is active
// and that they can access memory correctly

#define _GNU_SOURCE
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
#define TEST_SIZE (4 * 1024 * 1024)  // 4MB
#define NUM_THREADS 4

volatile int threads_ready = 0;
volatile int threads_can_exit = 0;
unsigned char *shared_mem;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *thread_func(void *arg) {
    int thread_id = *(int*)arg;
    int i;
    int errors = 0;
    
    printf("  Thread %d: Started on CPU %d\n", thread_id, sched_getcpu());
    
    // Each thread writes to its own section
    size_t section_size = TEST_SIZE / NUM_THREADS;
    size_t start = thread_id * section_size;
    size_t end = start + section_size;
    
    // Write pattern
    printf("  Thread %d: Writing pattern to section [%zu - %zu]\n", 
           thread_id, start, end);
    for (i = start; i < end; i += 4096) {  // Every page
        shared_mem[i] = (unsigned char)(0xA0 + thread_id);
    }
    
    // Verify pattern
    printf("  Thread %d: Verifying pattern...\n", thread_id);
    for (i = start; i < end; i += 4096) {
        if (shared_mem[i] != (unsigned char)(0xA0 + thread_id)) {
            errors++;
            if (errors < 5) {
                printf("  Thread %d: ERROR at offset %d: expected 0x%02x, got 0x%02x\n",
                       thread_id, i, (0xA0 + thread_id), shared_mem[i]);
            }
        }
    }
    
    if (errors == 0) {
        printf("  Thread %d: Memory access OK (%zu pages checked)\n", 
               thread_id, section_size / 4096);
    } else {
        printf("  Thread %d: FAILED with %d errors\n", thread_id, errors);
    }
    
    pthread_mutex_lock(&mutex);
    threads_ready++;
    pthread_mutex_unlock(&mutex);
    
    // Wait for signal to exit
    while (!threads_can_exit) {
        usleep(1000);
    }
    
    printf("  Thread %d: Exiting\n", thread_id);
    return (void*)(long)errors;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    int ret;
    int test_passed = 1;
    int i;
    long thread_errors[NUM_THREADS];
    
    printf("Test 18: Thread Creation With Replication Enabled\n");
    printf("==================================================\n");
    
    // Allocate memory
    shared_mem = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared_mem == MAP_FAILED) {
        printf("FAIL: mmap failed\n");
        return 1;
    }
    
    // Write initial pattern
    printf("Writing initial pattern...\n");
    memset(shared_mem, 0x55, TEST_SIZE);
    
    // Enable replication BEFORE creating threads
    printf("\nEnabling replication...\n");
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        munmap(shared_mem, TEST_SIZE);
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%x\n", ret);
    if (ret <= 0) {
        printf("FAIL: Replication not enabled (mask=0x%x)\n", ret);
        test_passed = 0;
        goto cleanup;
    }
    
    // Now create threads WITH replication active
    printf("\nCreating %d threads with replication enabled...\n", NUM_THREADS);
    for (i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        ret = pthread_create(&threads[i], NULL, thread_func, &thread_ids[i]);
        if (ret != 0) {
            printf("FAIL: pthread_create failed for thread %d: %s\n", 
                   i, strerror(ret));
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("All threads created successfully\n");
    
    // Check if replication is STILL enabled with threads running
    printf("\nChecking replication status with threads running...\n");
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret > 0) {
        printf("OK: Replication still enabled with threads (mask=0x%x)\n", ret);
    } else {
        printf("FAIL: Replication was disabled when threads created (mask=0x%x)\n", ret);
        test_passed = 0;
    }
    
    // Wait for all threads to be ready
    printf("\nWaiting for threads to complete work...\n");
    while (threads_ready < NUM_THREADS) {
        usleep(10000);
    }
    printf("All threads completed their work\n");
    
    // Signal threads to exit
    threads_can_exit = 1;
    
    // Join all threads
    printf("\nJoining threads...\n");
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], (void**)&thread_errors[i]);
        if (thread_errors[i] != 0) {
            printf("Thread %d reported %ld errors\n", i, thread_errors[i]);
            test_passed = 0;
        }
    }
    printf("All threads joined\n");
    
    // Verify replication still enabled after threads exit
    printf("\nChecking replication status after threads exit...\n");
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication status: 0x%x\n", ret);
    
    // Verify each thread's section
    printf("\nVerifying all thread sections from main...\n");
    int total_errors = 0;
    for (i = 0; i < NUM_THREADS; i++) {
        size_t section_size = TEST_SIZE / NUM_THREADS;
        size_t start = i * section_size;
        size_t check_offset = start;  // Check first page of each section
        
        if (shared_mem[check_offset] != (unsigned char)(0xA0 + i)) {
            printf("Section %d: ERROR - expected 0x%02x, got 0x%02x\n",
                   i, (0xA0 + i), shared_mem[check_offset]);
            total_errors++;
            test_passed = 0;
        }
    }
    if (total_errors == 0) {
        printf("All sections verified OK\n");
    }
    
cleanup:
    // Disable replication
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    munmap(shared_mem, TEST_SIZE);
    
    if (test_passed) {
        printf("\n✓ Test 18 PASSED: Threads work correctly with replication\n");
        return 0;
    } else {
        printf("\n✗ Test 18 FAILED: Issues with threads and replication\n");
        return 1;
    }
}
