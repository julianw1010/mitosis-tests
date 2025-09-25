// test16.c - MADV_DONTNEED with Page Table Replication
// Tests that MADV_DONTNEED properly handles replicated page tables
// and doesn't leave stale entries in replicas

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <signal.h>
#include <setjmp.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define TEST_SIZE (4 * 1024 * 1024)  // 4MB to span multiple PMDs
#define PATTERN1 0xDEADBEEF
#define PATTERN2 0xCAFEBABE

static sigjmp_buf jmpbuf;
static int segv_triggered = 0;

static void segv_handler(int sig) {
    segv_triggered = 1;
    siglongjmp(jmpbuf, 1);
}

int main(void) {
    char *ptr;
    unsigned int *uptr;
    int ret, i;
    int test_passed = 1;
    
    printf("Test 16: MADV_DONTNEED with Page Table Replication\n");
    printf("==================================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication\n");
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%x\n", ret);
    
    // Allocate memory
    ptr = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        printf("FAIL: mmap failed\n");
        return 1;
    }
    uptr = (unsigned int *)ptr;
    
    // Write pattern to ensure pages are allocated
    printf("Writing initial pattern...\n");
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        uptr[i] = PATTERN1;
    }
    
    // Verify initial write
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        if (uptr[i] != PATTERN1) {
            printf("FAIL: Initial pattern mismatch at offset %d\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Initial pattern verified\n");
    
    // Use MADV_DONTNEED to discard pages
    printf("Calling madvise(MADV_DONTNEED)...\n");
    ret = madvise(ptr, TEST_SIZE, MADV_DONTNEED);
    if (ret != 0) {
        printf("FAIL: madvise failed\n");
        test_passed = 0;
        goto cleanup;
    }
    
    // Reading should return zeros (anonymous memory behavior)
    printf("Verifying pages are zeroed after MADV_DONTNEED...\n");
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        if (uptr[i] != 0) {
            printf("FAIL: Non-zero value at offset %d: 0x%x\n", i, uptr[i]);
            printf("  This suggests stale data in replicated page tables\n");
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Pages properly zeroed\n");
    
    // Write new pattern
    printf("Writing new pattern...\n");
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        uptr[i] = PATTERN2;
    }
    
    // Verify new pattern works
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        if (uptr[i] != PATTERN2) {
            printf("FAIL: New pattern mismatch at offset %d\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("New pattern verified\n");
    
    // Do partial MADV_DONTNEED
    printf("Testing partial MADV_DONTNEED (first half)...\n");
    ret = madvise(ptr, TEST_SIZE/2, MADV_DONTNEED);
    if (ret != 0) {
        printf("FAIL: Partial madvise failed\n");
        test_passed = 0;
        goto cleanup;
    }
    
    // First half should be zero, second half should have pattern
    for (i = 0; i < TEST_SIZE/sizeof(int)/2; i++) {
        if (uptr[i] != 0) {
            printf("FAIL: First half not zeroed at offset %d\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    for (i = TEST_SIZE/sizeof(int)/2; i < TEST_SIZE/sizeof(int); i++) {
        if (uptr[i] != PATTERN2) {
            printf("FAIL: Second half corrupted at offset %d\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Partial MADV_DONTNEED handled correctly\n");
    
    // Test with mprotect + MADV_DONTNEED
    printf("Testing mprotect(NONE) + MADV_DONTNEED...\n");
    ret = mprotect(ptr, TEST_SIZE, PROT_NONE);
    if (ret != 0) {
        printf("FAIL: mprotect(NONE) failed\n");
        test_passed = 0;
        goto cleanup;
    }
    
    ret = madvise(ptr, TEST_SIZE, MADV_DONTNEED);
    if (ret != 0) {
        printf("FAIL: madvise on PROT_NONE memory failed\n");
        test_passed = 0;
        goto cleanup;
    }
    
    // Re-enable access
    ret = mprotect(ptr, TEST_SIZE, PROT_READ | PROT_WRITE);
    if (ret != 0) {
        printf("FAIL: mprotect restore failed\n");
        test_passed = 0;
        goto cleanup;
    }
    
    // Should be all zeros
    for (i = 0; i < TEST_SIZE/sizeof(int); i++) {
        if (uptr[i] != 0) {
            printf("FAIL: Memory not zeroed after NONE+DONTNEED at offset %d\n", i);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("mprotect + MADV_DONTNEED handled correctly\n");
    
cleanup:
    munmap(ptr, TEST_SIZE);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    if (test_passed) {
        printf("\n✓ Test 16 PASSED: MADV_DONTNEED correctly handled with replication\n");
        return 0;
    } else {
        printf("\n✗ Test 16 FAILED: Issues with MADV_DONTNEED and replicated pages\n");
        return 1;
    }
}
