// test32.c - Signal delivery during page faults test
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <setjmp.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

static sigjmp_buf jmpbuf;
static volatile int signal_count = 0;
static volatile int in_handler = 0;

void sigsegv_handler(int sig) {
    signal_count++;
    in_handler = 1;
    siglongjmp(jmpbuf, 1);
}

void sigbus_handler(int sig) {
    signal_count++;
    in_handler = 1;
    siglongjmp(jmpbuf, 2);
}

int main(void) {
    int ret;
    void *test_mem;
    struct sigaction sa_segv, sa_bus, old_segv, old_bus;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    if (numa_num_configured_nodes() < 2) {
        printf("SKIP: Need at least 2 NUMA nodes\n");
        return 0;
    }
    
    // Setup signal handlers
    memset(&sa_segv, 0, sizeof(sa_segv));
    sa_segv.sa_handler = sigsegv_handler;
    sigemptyset(&sa_segv.sa_mask);
    sa_segv.sa_flags = 0;
    
    memset(&sa_bus, 0, sizeof(sa_bus));
    sa_bus.sa_handler = sigbus_handler;
    sigemptyset(&sa_bus.sa_mask);
    sa_bus.sa_flags = 0;
    
    if (sigaction(SIGSEGV, &sa_segv, &old_segv) < 0) {
        printf("FAIL: Could not install SIGSEGV handler: %s\n", strerror(errno));
        return 1;
    }
    
    if (sigaction(SIGBUS, &sa_bus, &old_bus) < 0) {
        printf("FAIL: Could not install SIGBUS handler: %s\n", strerror(errno));
        sigaction(SIGSEGV, &old_segv, NULL);
        return 1;
    }
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Verify enabled
    unsigned long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication not enabled\n");
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Test 1: Access to unmapped memory (should trigger SIGSEGV)
    printf("Testing SIGSEGV on unmapped memory access...\n");
    signal_count = 0;
    
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // Try to access unmapped memory
        char *bad_ptr = (char*)0x1000000000;  // Likely unmapped address
        *bad_ptr = 42;  // Should trigger SIGSEGV
        
        printf("FAIL: No signal received for unmapped memory\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    if (signal_count != 1) {
        printf("FAIL: Expected 1 signal, got %d\n", signal_count);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    in_handler = 0;
    
    // Test 2: mprotect with PROT_NONE, then access
    printf("Testing SIGSEGV with PROT_NONE memory...\n");
    test_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (test_mem == MAP_FAILED) {
        printf("FAIL: mmap failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Write initial data
    memset(test_mem, 0xAA, 4096);
    
    // Make it inaccessible
    if (mprotect(test_mem, 4096, PROT_NONE) < 0) {
        printf("FAIL: mprotect failed: %s\n", strerror(errno));
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    signal_count = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // Try to read from PROT_NONE memory
        char val = *(char*)test_mem;  // Should trigger SIGSEGV
        (void)val;  // Avoid unused warning
        
        printf("FAIL: No signal for PROT_NONE access\n");
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    if (signal_count != 1) {
        printf("FAIL: Expected 1 signal for PROT_NONE, got %d\n", signal_count);
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    in_handler = 0;
    
    // Restore access and verify memory is intact
    if (mprotect(test_mem, 4096, PROT_READ | PROT_WRITE) < 0) {
        printf("FAIL: Could not restore protection: %s\n", strerror(errno));
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Verify data is still there
    if (((char*)test_mem)[0] != (char)0xAA || ((char*)test_mem)[4095] != (char)0xAA) {
        printf("FAIL: Memory corrupted after signal handling\n");
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    munmap(test_mem, 4096);
    
    // Test 3: Write to read-only memory
    printf("Testing SIGSEGV on write to read-only memory...\n");
    test_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (test_mem == MAP_FAILED) {
        printf("FAIL: mmap failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Write initial data
    memset(test_mem, 0xBB, 4096);
    
    // Make it read-only
    if (mprotect(test_mem, 4096, PROT_READ) < 0) {
        printf("FAIL: mprotect to PROT_READ failed: %s\n", strerror(errno));
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Reading should work
    char read_val = *(char*)test_mem;
    if (read_val != (char)0xBB) {
        printf("FAIL: Read from read-only memory failed\n");
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    signal_count = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // Try to write to read-only memory
        *(char*)test_mem = 0xCC;  // Should trigger SIGSEGV
        
        printf("FAIL: No signal for write to read-only memory\n");
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    if (signal_count != 1) {
        printf("FAIL: Expected 1 signal for read-only write, got %d\n", signal_count);
        munmap(test_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    in_handler = 0;
    
    munmap(test_mem, 4096);
    
    // Verify replication still enabled after signal handling
    status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication disabled after signal handling\n");
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGBUS, &old_bus, NULL);
        return 1;
    }
    
    // Cleanup
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    
    printf("PASS: Signal delivery during page faults test completed successfully\n");
    return 0;
}
