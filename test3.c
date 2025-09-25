#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

volatile int thread_running = 0;

void *thread_func(void *arg) {
    thread_running = 1;
    // Keep thread alive
    while (thread_running) {
        usleep(10000); // 10ms
    }
    return NULL;
}

int main(void) {
    pthread_t thread;
    long ret;
    
    printf("TEST3: Multi-threaded Process Rejection Test\n");
    printf("=============================================\n");
    
    // First, verify we can enable when single-threaded
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Cannot enable when single-threaded: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Can enable when single-threaded\n");
    
    // Disable it
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Cannot disable: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Disabled replication\n");
    
    // Create a thread
    if (pthread_create(&thread, NULL, thread_func, NULL) != 0) {
        printf("FAIL: pthread_create failed\n");
        return 1;
    }
    
    // Wait for thread to start
    while (!thread_running) {
        usleep(1000);
    }
    printf("INFO: Thread created and running\n");
    
    // Now try to enable replication - should fail with EBUSY
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret == 0) {
        printf("FAIL: Should NOT allow enabling with multiple threads!\n");
        thread_running = 0;
        pthread_join(thread, NULL);
        return 1;
    }
    if (errno != EBUSY) {
        printf("FAIL: Expected EBUSY error, got: %s\n", strerror(errno));
        thread_running = 0;
        pthread_join(thread, NULL);
        return 1;
    }
    printf("PASS: Correctly rejected with EBUSY when multi-threaded\n");
    
    // Verify it's still disabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret != 0) {
        printf("FAIL: Replication should still be disabled, got %ld\n", ret);
        thread_running = 0;
        pthread_join(thread, NULL);
        return 1;
    }
    printf("PASS: Replication remains disabled\n");
    
    // Clean up thread
    thread_running = 0;
    pthread_join(thread, NULL);
    printf("INFO: Thread terminated\n");
    
    // Now should be able to enable again (single-threaded)
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Cannot re-enable after thread termination: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Can enable again when back to single-threaded\n");
    
    // Clean up
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST3: SUCCESS - Multi-threaded rejection works correctly\n");
    return 0;
}
