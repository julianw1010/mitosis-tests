// test15.c - Aggressive mremap() stress test to expose replication bugs
// Modified version with additional debug output for hang investigation

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

#define PAGE_SIZE 4096
#define PMD_SIZE (512 * PAGE_SIZE)  // 2MB
#define PUD_SIZE (512 * PMD_SIZE)   // 1GB

int main(void) {
    int ret;
    void *addr, *new_addr;
    char *ptr;
    
    printf("Test15: Aggressive mremap() stress test\n");
    printf("========================================\n\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%lx\n\n", mask);
    
    // Test 1: Rapid size oscillation
    printf("Test 1: Rapid size oscillation\n");
    size_t sizes[] = {4*PAGE_SIZE, 100*PAGE_SIZE, 2*PAGE_SIZE, 513*PAGE_SIZE, PAGE_SIZE};
    
    addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        printf("FAIL: Initial mmap failed\n");
        return 1;
    }
    
    size_t current_size = PAGE_SIZE;
    for (int i = 0; i < 5; i++) {
        size_t new_size = sizes[i];
        printf("  Resizing %zu -> %zu bytes...", current_size, new_size);
        
        new_addr = mremap(addr, current_size, new_size, MREMAP_MAYMOVE);
        if (new_addr == MAP_FAILED) {
            printf(" FAILED: %s\n", strerror(errno));
            return 1;
        }
        
        addr = new_addr;
        current_size = new_size;
        
        // Write to first and last page
        ptr = (char*)addr;
        ptr[0] = i;
        ptr[current_size - 1] = i;
        
        if (ptr[0] != i || ptr[current_size - 1] != i) {
            printf(" FAIL: Write verification failed\n");
            return 1;
        }
        printf(" OK\n");
    }
    munmap(addr, current_size);
    printf("\n");
    
    // Test 2: Cross multiple PMD boundaries in one shot
    printf("Test 2: Large expansion crossing multiple PMDs\n");
    size_t initial = 10 * PAGE_SIZE;
    size_t final = 3 * PMD_SIZE + 100 * PAGE_SIZE;  // >3 PMDs
    
    addr = mmap(NULL, initial, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        printf("FAIL: mmap failed\n");
        return 1;
    }
    
    printf("  Expanding from %zu bytes to %zu bytes (%.2f PMDs)...",
           initial, final, (float)final/PMD_SIZE);
    
    new_addr = mremap(addr, initial, final, MREMAP_MAYMOVE);
    if (new_addr == MAP_FAILED) {
        printf(" FAILED: %s\n", strerror(errno));
        return 1;
    }
    addr = new_addr;
    ptr = (char*)addr;
    
    // Write at PMD boundaries
    int test_offsets[] = {
        0,
        PMD_SIZE - PAGE_SIZE,
        PMD_SIZE,
        PMD_SIZE + PAGE_SIZE,
        2*PMD_SIZE - PAGE_SIZE,
        2*PMD_SIZE,
        2*PMD_SIZE + PAGE_SIZE,
        3*PMD_SIZE - PAGE_SIZE,
        3*PMD_SIZE
    };
    
    for (int i = 0; i < 9; i++) {
        size_t offset = test_offsets[i];
        if (offset >= final) continue;
        
        ptr[offset] = 0x42 + i;
        if (ptr[offset] != (0x42 + i)) {
            printf(" FAIL at offset %zu (PMD boundary test)\n", offset);
            return 1;
        }
    }
    printf(" OK\n");
    munmap(addr, final);
    printf("\n");
    
    // Test 3: mremap to fixed address
    printf("Test 3: mremap with MREMAP_FIXED (force specific addresses)\n");
    
    void *region1 = mmap(NULL, PMD_SIZE, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *region2 = mmap(NULL, PMD_SIZE, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (region1 == MAP_FAILED || region2 == MAP_FAILED) {
        printf("  Could not allocate test regions\n");
    } else {
        munmap(region1, PMD_SIZE);
        munmap(region2, PMD_SIZE);
        
        addr = mmap(region1, PMD_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (addr != region1) {
            printf("  Could not map at fixed address\n");
        } else {
            ptr = (char*)addr;
            memset(ptr, 0x33, PMD_SIZE);
            
            printf("  Moving from %p to %p...", region1, region2);
            new_addr = mremap(addr, PMD_SIZE, PMD_SIZE,
                            MREMAP_MAYMOVE | MREMAP_FIXED, region2);
            
            if (new_addr == MAP_FAILED) {
                printf(" FAILED: %s\n", strerror(errno));
            } else if (new_addr != region2) {
                printf(" FAILED: Wrong address\n");
                return 1;
            } else {
                ptr = (char*)new_addr;
                if (ptr[0] != 0x33 || ptr[PMD_SIZE-1] != 0x33) {
                    printf(" FAIL: Data not preserved\n");
                    return 1;
                }
                printf(" OK\n");
                munmap(new_addr, PMD_SIZE);
            }
        }
    }
    printf("\n");
    
    // Test 4: Shrink then immediately expand (stress replica cleanup/recreation)
    printf("Test 4: Rapid shrink/expand cycles\n");
    
    addr = mmap(NULL, PMD_SIZE * 2, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        printf("FAIL: mmap failed\n");
        return 1;
    }
    
    ptr = (char*)addr;
    for (int cycle = 0; cycle < 5; cycle++) {
        printf("  Cycle %d: ", cycle);
        
        // Fill with pattern
        for (size_t i = 0; i < PMD_SIZE * 2; i += PAGE_SIZE) {
            ptr[i] = cycle;
        }
        
        // Shrink to small
        new_addr = mremap(addr, PMD_SIZE * 2, PAGE_SIZE * 10, 0);
        if (new_addr == MAP_FAILED) {
            printf("shrink failed\n");
            return 1;
        }
        addr = new_addr;
        ptr = (char*)addr;
        
        // Immediately expand back
        new_addr = mremap(addr, PAGE_SIZE * 10, PMD_SIZE * 2, MREMAP_MAYMOVE);
        if (new_addr == MAP_FAILED) {
            printf("expand failed\n");
            return 1;
        }
        addr = new_addr;
        ptr = (char*)addr;
        
        // Verify first pages still have data
        if (ptr[0] != cycle) {
            printf("FAIL: Lost data after shrink/expand\n");
            return 1;
        }
        
        // Write to expanded region with debug
        for (size_t i = PAGE_SIZE * 10; i < PMD_SIZE * 2; i += PAGE_SIZE) {
            // ADD DETAILED DEBUG
            if (i == PAGE_SIZE * 10) {  // First write to expanded area
                printf("\n    DEBUG: About to write to offset %zu (addr=%p)\n", i, &ptr[i]);
                printf("    DEBUG: Writing value 0x%02x\n", cycle + 0x80);
                fflush(stdout);
            }
            
            ptr[i] = cycle + 0x80;
            
            if (i == PAGE_SIZE * 10) {
                printf("    DEBUG: Write completed, now reading back...\n");
                fflush(stdout);
            }
            
            unsigned char read_val = ptr[i];
            if (read_val != (cycle + 0x80)) {
                printf("\n    FAIL: Cannot write to expanded region at offset %zu\n", i);
                printf("    Expected: 0x%02x, Got: 0x%02x\n", cycle + 0x80, read_val);
                printf("    Address: %p\n", &ptr[i]);
                return 1;
            }
            
            if (i == PAGE_SIZE * 10) {
                printf("    DEBUG: Read back successful, value=0x%02x\n", read_val);
                fflush(stdout);
            }
        }
        printf("OK\n");
    }
    munmap(addr, PMD_SIZE * 2);
    printf("\n");
    
    // Test 5: Testing error handling with bad parameters
    printf("Test 5: Testing error handling with bad parameters\n");
    
    addr = mmap(NULL, PAGE_SIZE * 10, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr != MAP_FAILED) {
        // Try unaligned size
        new_addr = mremap(addr, PAGE_SIZE * 10, PAGE_SIZE * 10 + 1, MREMAP_MAYMOVE);
        if (new_addr != MAP_FAILED) {
            printf("  WARNING: Unaligned size accepted\n");
            munmap(new_addr, PAGE_SIZE * 11);
        } else {
            printf("  Unaligned size correctly rejected\n");
            munmap(addr, PAGE_SIZE * 10);
        }
    }
    
    // Final check
    mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (mask == 0) {
        printf("\nFAIL: Replication got disabled during tests!\n");
        return 1;
    }
    
    // Disable replication with detailed debug
    printf("\nAbout to disable replication...\n");
    fflush(stdout);
    
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("prctl returned %d\n", ret);
    fflush(stdout);
    
    if (ret < 0) {
        printf("FAIL: Could not disable replication: %s\n", strerror(errno));
        return 1;
    }
    
    printf("Replication disabled successfully\n");
    fflush(stdout);
    
    printf("\n========================================\n");
    printf("PASS: All aggressive mremap tests passed\n");
    printf("========================================\n");
    
    printf("About to exit program...\n");
    fflush(stdout);
    
    return 0;
}
