#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    void *initial_brk, *current_brk, *new_brk;
    char *heap_ptr;
    size_t increment;
    int i;
    
    printf("TEST12: Heap Expansion (brk/sbrk) Test\n");
    printf("=======================================\n");
    
    // Get initial heap boundary
    initial_brk = sbrk(0);
    if (initial_brk == (void *)-1) {
        printf("FAIL: Cannot get initial brk: %s\n", strerror(errno));
        return 1;
    }
    printf("INFO: Initial brk at %p\n", initial_brk);
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Test 1: Small heap expansion
    increment = 4096;  // One page
    new_brk = sbrk(increment);
    if (new_brk == (void *)-1) {
        printf("FAIL: sbrk failed for %zu bytes: %s\n", increment, strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Expanded heap by %zu bytes\n", increment);
    
    // Write to newly allocated heap space
    heap_ptr = (char *)new_brk;
    for (i = 0; i < increment; i++) {
        heap_ptr[i] = (char)(i % 256);
    }
    printf("PASS: Wrote pattern to expanded heap\n");
    
    // Verify the pattern
    for (i = 0; i < increment; i++) {
        if (heap_ptr[i] != (char)(i % 256)) {
            printf("FAIL: Pattern mismatch at offset %d\n", i);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
    }
    printf("PASS: Pattern verified in heap\n");
    
    // Test 2: Larger heap expansion
    increment = 4096 * 10;  // 10 pages
    new_brk = sbrk(increment);
    if (new_brk == (void *)-1) {
        printf("FAIL: Large sbrk failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Expanded heap by %zu more bytes\n", increment);
    
    // Write different pattern to new area
    heap_ptr = (char *)new_brk;
    memset(heap_ptr, 'X', increment);
    heap_ptr[0] = 'S';
    heap_ptr[increment - 1] = 'E';
    
    if (heap_ptr[0] != 'S' || heap_ptr[increment - 1] != 'E') {
        printf("FAIL: Large heap area write/read failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Large heap expansion works\n");
    
    // Test 3: Use brk() to set specific heap end
    current_brk = sbrk(0);
    new_brk = (char *)current_brk + 4096;
    if (brk(new_brk) != 0) {
        printf("FAIL: brk() failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Verify brk was set correctly
    current_brk = sbrk(0);
    if (current_brk != new_brk) {
        printf("FAIL: brk not set correctly (expected %p, got %p)\n",
               new_brk, current_brk);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: brk() set heap boundary correctly\n");
    
    // Test 4: Shrink heap
    new_brk = (char *)current_brk - 4096;
    if (brk(new_brk) != 0) {
        printf("WARN: Cannot shrink heap (may be system limitation)\n");
    } else {
        current_brk = sbrk(0);
        if (current_brk == new_brk) {
            printf("PASS: Heap shrunk successfully\n");
        } else {
            printf("INFO: Heap shrink attempted but boundary is %p\n", current_brk);
        }
    }
    
    // Test 5: Allocate via malloc (which uses brk for small allocations)
    void *malloc_ptr = malloc(1024);
    if (!malloc_ptr) {
        printf("FAIL: malloc failed after brk operations\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    strcpy(malloc_ptr, "MallocTest");
    if (strcmp(malloc_ptr, "MallocTest") != 0) {
        printf("FAIL: malloc'd memory not working correctly\n");
        free(malloc_ptr);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: malloc works after brk operations\n");
    free(malloc_ptr);
    
    // Check final heap position
    current_brk = sbrk(0);
    ptrdiff_t total_growth = (char *)current_brk - (char *)initial_brk;
    printf("INFO: Total heap growth: %ld bytes\n", (long)total_growth);
    
    // Verify replication still enabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Replication disabled after heap operations\n");
        return 1;
    }
    printf("PASS: Replication still enabled (0x%lx)\n", ret);
    
    // Clean up
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST12: SUCCESS - Heap expansion works with replication\n");
    return 0;
}
