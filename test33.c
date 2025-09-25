// test31.c - Minimal MADV_DONTNEED test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define PAGE_SIZE 4096
#define NUM_PAGES 10  // Just test 10 pages
#define TEST_SIZE (NUM_PAGES * PAGE_SIZE)
#define PATTERN 0xDEADBEEF

int main(void) {
    char *ptr;
    unsigned int *uptr;
    int ret, i, page;
    
    printf("Minimal MADV_DONTNEED Test (%d pages)\n", NUM_PAGES);
    printf("=====================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("Could not enable replication\n");
        return 1;
    }
    printf("Replication enabled: 0x%x\n", prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0));
    
    ptr = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    uptr = (unsigned int *)ptr;
    
    // Test each page individually
    for (page = 0; page < NUM_PAGES; page++) {
        int offset = page * PAGE_SIZE / sizeof(int);
        
        printf("Testing page %d...\n", page);
        
        // Write pattern to this page
        for (i = 0; i < PAGE_SIZE/sizeof(int); i++) {
            uptr[offset + i] = PATTERN;
        }
        
        // Verify pattern
        if (uptr[offset] != PATTERN) {
            printf("  ERROR: Pattern not written\n");
            return 1;
        }
        
        // Discard this page
        ret = madvise(ptr + page * PAGE_SIZE, PAGE_SIZE, MADV_DONTNEED);
        if (ret != 0) {
            perror("  madvise");
            return 1;
        }
        
        // Check first int of page (this will trigger page fault)
        if (uptr[offset] != 0) {
            printf("  ERROR: Page %d not zeroed, got 0x%x\n", page, uptr[offset]);
            return 1;
        }
        
        printf("  Page %d: OK (zeroed after MADV_DONTNEED)\n", page);
    }
    
    munmap(ptr, TEST_SIZE);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\n✓ Test PASSED: All %d pages handled correctly\n", NUM_PAGES);
    return 0;
}
