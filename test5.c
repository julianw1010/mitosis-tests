#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    void *mem1, *mem2;
    char *ptr;
    int i;
    
    printf("TEST5: Memory Allocation with Replication Test\n");
    printf("===============================================\n");
    
    // Enable replication first
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Test 1: Allocate memory with malloc (heap)
    mem1 = malloc(4096 * 10);  // 10 pages
    if (!mem1) {
        printf("FAIL: malloc failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Allocated 40KB via malloc\n");
    
    // Write to the memory to trigger page faults
    ptr = (char *)mem1;
    for (i = 0; i < 10; i++) {
        ptr[i * 4096] = 'A' + i;  // Write to each page
    }
    printf("PASS: Wrote to malloc'd memory (triggered page faults)\n");
    
    // Verify reads work correctly
    for (i = 0; i < 10; i++) {
        if (ptr[i * 4096] != 'A' + i) {
            printf("FAIL: Read incorrect value at page %d: expected '%c', got '%c'\n",
                   i, 'A' + i, ptr[i * 4096]);
            free(mem1);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
    }
    printf("PASS: Read back correct values from malloc'd memory\n");
    
    // Test 2: Allocate memory with mmap (anonymous)
    mem2 = mmap(NULL, 4096 * 20, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem2 == MAP_FAILED) {
        printf("FAIL: mmap failed: %s\n", strerror(errno));
        free(mem1);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Allocated 80KB via mmap\n");
    
    // Write pattern to mmap'd memory
    ptr = (char *)mem2;
    for (i = 0; i < 20; i++) {
        ptr[i * 4096] = 'a' + (i % 26);
    }
    printf("PASS: Wrote to mmap'd memory\n");
    
    // Verify mmap'd memory
    for (i = 0; i < 20; i++) {
        if (ptr[i * 4096] != 'a' + (i % 26)) {
            printf("FAIL: mmap read incorrect at page %d\n", i);
            free(mem1);
            munmap(mem2, 4096 * 20);
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
    }
    printf("PASS: Read back correct values from mmap'd memory\n");
    
    // Test 3: Large allocation spanning multiple PMDs
    void *large_mem = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (large_mem == MAP_FAILED) {
        printf("WARN: Could not allocate 2MB (may be system limit)\n");
    } else {
        // Write to first and last page
        ((char *)large_mem)[0] = 'X';
        ((char *)large_mem)[2 * 1024 * 1024 - 1] = 'Y';
        
        if (((char *)large_mem)[0] == 'X' && 
            ((char *)large_mem)[2 * 1024 * 1024 - 1] == 'Y') {
            printf("PASS: Large allocation works with replication\n");
        } else {
            printf("FAIL: Large allocation read/write failed\n");
        }
        munmap(large_mem, 2 * 1024 * 1024);
    }
    
    // Clean up
    free(mem1);
    munmap(mem2, 4096 * 20);
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not disable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication disabled\n");
    
    printf("\nTEST5: SUCCESS - Memory operations work with replication\n");
    return 0;
}
