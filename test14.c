// test14.c - Test munmap() with page table replication enabled
// Tests whether unmapping memory regions correctly handles replica cleanup

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <errno.h>
#include <stdint.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

#define PAGE_SIZE 4096
#define TEST_SIZE (256 * PAGE_SIZE)  // 1MB test region

int main(void) {
    int ret;
    void *addr1, *addr2, *addr3;
    char pattern[PAGE_SIZE];
    
    printf("Test14: munmap() with page table replication\n");
    printf("=============================================\n\n");
    
    // Enable replication on all nodes
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%lx\n", mask);
    
    // Allocate three memory regions
    addr1 = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr1 == MAP_FAILED) {
        printf("FAIL: mmap addr1 failed: %s\n", strerror(errno));
        return 1;
    }
    printf("Mapped region 1: %p - %p\n", addr1, addr1 + TEST_SIZE);
    
    addr2 = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr2 == MAP_FAILED) {
        printf("FAIL: mmap addr2 failed: %s\n", strerror(errno));
        return 1;
    }
    printf("Mapped region 2: %p - %p\n", addr2, addr2 + TEST_SIZE);
    
    addr3 = mmap(NULL, TEST_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr3 == MAP_FAILED) {
        printf("FAIL: mmap addr3 failed: %s\n", strerror(errno));
        return 1;
    }
    printf("Mapped region 3: %p - %p\n\n", addr3, addr3 + TEST_SIZE);
    
    // Write distinctive patterns to each region
    memset(pattern, 0xAA, PAGE_SIZE);
    for (int i = 0; i < TEST_SIZE; i += PAGE_SIZE) {
        memcpy(addr1 + i, pattern, PAGE_SIZE);
    }
    
    memset(pattern, 0xBB, PAGE_SIZE);
    for (int i = 0; i < TEST_SIZE; i += PAGE_SIZE) {
        memcpy(addr2 + i, pattern, PAGE_SIZE);
    }
    
    memset(pattern, 0xCC, PAGE_SIZE);
    for (int i = 0; i < TEST_SIZE; i += PAGE_SIZE) {
        memcpy(addr3 + i, pattern, PAGE_SIZE);
    }
    
    printf("Written patterns to all regions\n");
    
    // Verify patterns before unmapping
    if (*(unsigned char*)addr1 != 0xAA) {
        printf("FAIL: Region 1 pattern incorrect before unmap\n");
        return 1;
    }
    if (*(unsigned char*)addr2 != 0xBB) {
        printf("FAIL: Region 2 pattern incorrect before unmap\n");
        return 1;
    }
    if (*(unsigned char*)addr3 != 0xCC) {
        printf("FAIL: Region 3 pattern incorrect before unmap\n");
        return 1;
    }
    
    // Unmap middle region
    printf("Unmapping middle region (addr2)...\n");
    ret = munmap(addr2, TEST_SIZE);
    if (ret != 0) {
        printf("FAIL: munmap addr2 failed: %s\n", strerror(errno));
        return 1;
    }
    printf("Successfully unmapped region 2\n\n");
    
    // Verify remaining regions still accessible
    printf("Verifying remaining regions after unmap...\n");
    if (*(unsigned char*)addr1 != 0xAA) {
        printf("FAIL: Region 1 corrupted after unmapping region 2\n");
        return 1;
    }
    if (*(unsigned char*)addr3 != 0xCC) {
        printf("FAIL: Region 3 corrupted after unmapping region 2\n");
        return 1;
    }
    printf("Regions 1 and 3 still valid\n\n");
    
    // Try to map something new in the gap
    void *addr_new = mmap(addr2, TEST_SIZE/2, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (addr_new == MAP_FAILED) {
        // MAP_FIXED_NOREPLACE might not be supported, try regular MAP_FIXED
        addr_new = mmap(addr2, TEST_SIZE/2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    
    if (addr_new != MAP_FAILED) {
        printf("Successfully mapped new region in gap: %p\n", addr_new);
        memset(pattern, 0xDD, PAGE_SIZE);
        memcpy(addr_new, pattern, PAGE_SIZE);
        if (*(unsigned char*)addr_new != 0xDD) {
            printf("FAIL: New region not writable\n");
            return 1;
        }
        printf("New region is functional\n\n");
    } else {
        printf("Note: Could not map in gap (kernel may have randomized it)\n\n");
    }
    
    // Partial unmap - unmap only part of region 3
    printf("Partial unmap: unmapping second half of region 3...\n");
    ret = munmap(addr3 + TEST_SIZE/2, TEST_SIZE/2);
    if (ret != 0) {
        printf("FAIL: Partial munmap failed: %s\n", strerror(errno));
        return 1;
    }
    
    // First half should still be accessible
    if (*(unsigned char*)addr3 != 0xCC) {
        printf("FAIL: First half of region 3 corrupted after partial unmap\n");
        return 1;
    }
    printf("First half of region 3 still accessible\n\n");
    
    // Check replication is still enabled
    mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (mask == 0) {
        printf("FAIL: Replication disabled after munmap operations\n");
        return 1;
    }
    printf("Replication still enabled: 0x%lx\n", mask);
    
    // Final cleanup
    munmap(addr1, TEST_SIZE);
    munmap(addr3, TEST_SIZE/2);  // Only first half remains
    if (addr_new != MAP_FAILED) {
        munmap(addr_new, TEST_SIZE/2);
    }
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not disable replication: %s\n", strerror(errno));
        return 1;
    }
    
    printf("\n==============================================\n");
    printf("PASS: munmap() with replication works correctly\n");
    printf("- Full unmapping succeeded\n");
    printf("- Partial unmapping succeeded\n");
    printf("- Adjacent regions remained valid\n");
    printf("- Replication stayed enabled throughout\n");
    printf("==============================================\n");
    
    return 0;
}
