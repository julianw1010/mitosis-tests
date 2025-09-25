// test29.c - Debug version to find where it hangs
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define TEST_SIZE (4 * 1024 * 1024)  // Smaller: 4MB
#define PAGE_SIZE 4096
#define PATTERN 0xDEADBEEF

int main(void) {
    char *ptr;
    unsigned int *uptr;
    int ret, j;
    
    printf("MADV_DONTNEED Debug Test\n");
    printf("========================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("Could not enable replication\n");
        return 1;
    }
    printf("Replication enabled: 0x%x\n", prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0));
    
    // Allocate memory
    ptr = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    uptr = (unsigned int *)ptr;
    printf("Allocated %d MB at %p\n", TEST_SIZE/(1024*1024), ptr);
    
    // Write pattern
    printf("Writing pattern...\n");
    for (j = 0; j < TEST_SIZE/sizeof(int); j++) {
        uptr[j] = PATTERN;
        if (j % 100000 == 0) {
            printf("  Written %d/%d\n", j, TEST_SIZE/sizeof(int));
        }
    }
    printf("Pattern written\n");
    
    // Verify pattern
    printf("Verifying pattern...\n");
    for (j = 0; j < TEST_SIZE/sizeof(int); j++) {
        if (uptr[j] != PATTERN) {
            printf("Pattern mismatch at %d: got 0x%x\n", j, uptr[j]);
            return 1;
        }
    }
    printf("Pattern verified\n");
    
    // MADV_DONTNEED on first half
    int len = TEST_SIZE / 2;
    printf("Calling MADV_DONTNEED on first %d bytes...\n", len);
    ret = madvise(ptr, len, MADV_DONTNEED);
    if (ret != 0) {
        perror("madvise");
        return 1;
    }
    printf("MADV_DONTNEED completed\n");
    
    // Check first half should be zero
    printf("Checking first half for zeros...\n");
    int errors = 0;
    for (j = 0; j < len/sizeof(int); j++) {
        if (uptr[j] != 0) {
            printf("ERROR at offset %d: expected 0, got 0x%x\n", 
                   j * sizeof(int), uptr[j]);
            errors++;
            if (errors > 10) {
                printf("Too many errors, stopping check\n");
                break;
            }
        }
        if (j % 100000 == 0) {
            printf("  Checked %d/%d\n", j, len/sizeof(int));
        }
    }
    
    if (errors == 0) {
        printf("First half properly zeroed\n");
    }
    
    // Check second half still has pattern
    printf("Checking second half for pattern...\n");
    for (j = len/sizeof(int); j < TEST_SIZE/sizeof(int); j++) {
        if (uptr[j] != PATTERN) {
            printf("ERROR at offset %d: expected 0x%x, got 0x%x\n",
                   j * sizeof(int), PATTERN, uptr[j]);
            break;
        }
    }
    printf("Second half check complete\n");
    
    munmap(ptr, TEST_SIZE);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    if (errors == 0) {
        printf("\n✓ Test PASSED\n");
        return 0;
    } else {
        printf("\n✗ Test FAILED: %d errors\n", errors);
        return 1;
    }
}
