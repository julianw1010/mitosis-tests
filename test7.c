#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <setjmp.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

volatile sig_atomic_t signal_count = 0;
volatile sig_atomic_t sigusr1_received = 0;
volatile sig_atomic_t sigusr2_received = 0;
volatile sig_atomic_t sigalrm_received = 0;
sigjmp_buf jmpbuf;
char *global_mem = NULL;

void sighandler(int sig) {
    signal_count++;
    if (sig == SIGUSR1) {
        sigusr1_received = 1;
    } else if (sig == SIGUSR2) {
        sigusr2_received = 2;
    }
}

void sigalrm_handler(int sig) {
    sigalrm_received = 1;
    if (global_mem && strcmp(global_mem, "SignalTest") == 0) {
        strcat(global_mem, "OK");
    }
}

void sigsegv_handler(int sig) {
    printf("INFO: SIGSEGV caught as expected\n");
    siglongjmp(jmpbuf, 1);
}

int main(void) {
    long ret;
    struct sigaction sa;
    
    printf("TEST7: Signal Handling with Replication Test\n");
    printf("=============================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Test 1: Basic signal delivery
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("FAIL: sigaction SIGUSR1 failed\n");
        return 1;
    }
    if (sigaction(SIGUSR2, &sa, NULL) < 0) {
        printf("FAIL: sigaction SIGUSR2 failed\n");
        return 1;
    }
    printf("INFO: Signal handlers installed\n");
    
    // Send signals to ourselves
    kill(getpid(), SIGUSR1);
    usleep(10000); // Give signal time to be delivered
    if (!sigusr1_received) {
        printf("FAIL: SIGUSR1 not received\n");
        return 1;
    }
    printf("PASS: SIGUSR1 delivered correctly\n");
    
    kill(getpid(), SIGUSR2);
    usleep(10000);
    if (!sigusr2_received) {
        printf("FAIL: SIGUSR2 not received\n");
        return 1;
    }
    printf("PASS: SIGUSR2 delivered correctly\n");
    
    if (signal_count != 2) {
        printf("FAIL: Expected 2 signals, got %d\n", signal_count);
        return 1;
    }
    printf("PASS: Signal count correct (%d)\n", signal_count);
    
    // Test 2: Allocate memory and access it in signal handler
    global_mem = malloc(4096);
    if (!global_mem) {
        printf("FAIL: malloc failed\n");
        return 1;
    }
    strcpy(global_mem, "SignalTest");
    
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        printf("FAIL: sigaction SIGALRM failed\n");
        free(global_mem);
        return 1;
    }
    
    sigalrm_received = 0;
    alarm(1);
    
    // Wait for alarm
    int timeout = 0;
    while (!sigalrm_received && timeout < 30) {
        usleep(100000); // 100ms
        timeout++;
    }
    
    if (!sigalrm_received) {
        printf("FAIL: SIGALRM not received\n");
        free(global_mem);
        return 1;
    }
    
    if (strcmp(global_mem, "SignalTestOK") != 0) {
        printf("FAIL: Memory not updated correctly (got: %s)\n", global_mem);
        free(global_mem);
        return 1;
    }
    printf("PASS: Signal handler accessed memory correctly\n");
    
    // Test 3: SIGSEGV handling (tests fault path with replication)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGSEGV, &sa, NULL) < 0) {
        printf("FAIL: sigaction SIGSEGV failed\n");
        free(global_mem);
        return 1;
    }
    
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // This will trigger SIGSEGV
        printf("INFO: Triggering SIGSEGV...\n");
        volatile char *null_ptr = NULL;
        *null_ptr = 42;  // Should trigger SIGSEGV
        printf("FAIL: SIGSEGV not triggered\n");
        free(global_mem);
        return 1;
    } else {
        // We jumped here from signal handler
        printf("PASS: SIGSEGV handled correctly\n");
    }
    
    // Verify replication still enabled after signal handling
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Replication disabled after signals\n");
        free(global_mem);
        return 1;
    }
    printf("PASS: Replication still enabled after signals (0x%lx)\n", ret);
    
    // Clean up
    free(global_mem);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST7: SUCCESS - Signals work correctly with replication\n");
    return 0;
}
