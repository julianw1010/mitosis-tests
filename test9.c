#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

sigjmp_buf jmpbuf;
int segv_caught = 0;

void sigsegv_handler(int sig) {
    segv_caught = 1;
    siglongjmp(jmpbuf, 1);
}

int main(void) {
    long ret;
    void *mem;
    char *ptr;
    struct sigaction sa;
    
    printf("TEST9: Memory Protection (mprotect) Test\n");
    printf("=========================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Allocate memory
    mem = mmap(NULL, 4096 * 3, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        printf("FAIL: mmap failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    ptr = (char *)mem;
    printf("PASS: Allocated 3 pages of memory\n");
    
    // Test 1: Write to R/W memory
    strcpy(ptr, "ReadWrite");
    if (strcmp(ptr, "ReadWrite") != 0) {
        printf("FAIL: Initial write failed\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Can write to R/W memory\n");
    
    // Test 2: Change to read-only
    ret = mprotect(mem, 4096, PROT_READ);
    if (ret < 0) {
        printf("FAIL: mprotect to PROT_READ failed: %s\n", strerror(errno));
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Changed first page to read-only\n");
    
    // Test 3: Verify read still works
    if (strcmp(ptr, "ReadWrite") != 0) {
        printf("FAIL: Cannot read after mprotect\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Can still read from read-only page\n");
    
    // Test 4: Verify write protection (should trigger SIGSEGV)
    sa.sa_handler = sigsegv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    
    segv_caught = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // This should trigger SIGSEGV
        ptr[0] = 'X';
        printf("FAIL: Write to read-only page succeeded (should have segfaulted)\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    if (!segv_caught) {
        printf("FAIL: SIGSEGV not caught\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Write to read-only page correctly trapped\n");
    
    // Test 5: Change back to read-write
    ret = mprotect(mem, 4096, PROT_READ | PROT_WRITE);
    if (ret < 0) {
        printf("FAIL: mprotect back to R/W failed: %s\n", strerror(errno));
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Now write should work
    strcpy(ptr, "WritableAgain");
    if (strcmp(ptr, "WritableAgain") != 0) {
        printf("FAIL: Cannot write after restoring R/W\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Can write again after restoring R/W\n");
    
    // Test 6: PROT_NONE (no access)
    ret = mprotect(mem + 4096, 4096, PROT_NONE);
    if (ret < 0) {
        printf("FAIL: mprotect to PROT_NONE failed: %s\n", strerror(errno));
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Changed second page to PROT_NONE\n");
    
    segv_caught = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        // This should trigger SIGSEGV
        char c = ptr[4096];
        (void)c;
        printf("FAIL: Read from PROT_NONE page succeeded\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    if (!segv_caught) {
        printf("FAIL: SIGSEGV not caught for PROT_NONE\n");
        munmap(mem, 4096 * 3);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Access to PROT_NONE page correctly trapped\n");
    
    // Test 7: PROT_EXEC (if supported)
    ret = mprotect(mem + 4096*2, 4096, PROT_READ | PROT_EXEC);
    if (ret == 0) {
        printf("PASS: Set PROT_EXEC on third page\n");
    } else {
        printf("INFO: PROT_EXEC not supported (may be NX bit): %s\n", strerror(errno));
    }
    
    // Verify replication still enabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Replication disabled after mprotect operations\n");
        munmap(mem, 4096 * 3);
        return 1;
    }
    printf("PASS: Replication still enabled (0x%lx)\n", ret);
    
    // Clean up
    munmap(mem, 4096 * 3);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST9: SUCCESS - mprotect works correctly with replication\n");
    return 0;
}
