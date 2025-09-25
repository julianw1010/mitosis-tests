// test25.c - mlock/mlockall with replicated page tables
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <errno.h>
#include <stdint.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define PAGE_SIZE 4096
#define NUM_PAGES 32

int main(void) {
    void *region1, *region2, *region3;
    struct rlimit rlim;
    int pass = 1;
    int i;
    
    printf("Test 25: Memory Locking (mlock/mlockall) with Replication\n");
    printf("=========================================================\n");
    
    // Check and potentially increase RLIMIT_MEMLOCK
    if (getrlimit(RLIMIT_MEMLOCK, &rlim) == 0) {
        printf("Current RLIMIT_MEMLOCK: soft=%lu, hard=%lu\n", 
               rlim.rlim_cur, rlim.rlim_max);
        
        // Try to increase if too low
        if (rlim.rlim_cur < (NUM_PAGES * PAGE_SIZE * 3)) {
            rlim.rlim_cur = rlim.rlim_max;
            if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
                printf("WARNING: Could not increase RLIMIT_MEMLOCK\n");
            }
        }
    }
    
    // Allocate test regions
    region1 = mmap(NULL, PAGE_SIZE * NUM_PAGES, 
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region1 == MAP_FAILED) {
        perror("mmap region1");
        return 1;
    }
    
    region2 = mmap(NULL, PAGE_SIZE * NUM_PAGES,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region2 == MAP_FAILED) {
        perror("mmap region2");
        munmap(region1, PAGE_SIZE * NUM_PAGES);
        return 1;
    }
    
    printf("Allocated regions: region1=%p, region2=%p\n", region1, region2);
    
    // Fill with initial data
    memset(region1, 0xAA, PAGE_SIZE * NUM_PAGES);
    memset(region2, 0xBB, PAGE_SIZE * NUM_PAGES);
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        munmap(region1, PAGE_SIZE * NUM_PAGES);
        munmap(region2, PAGE_SIZE * NUM_PAGES);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled with mask: 0x%lx\n", mask);
    
    // Test 1: mlock specific region
    printf("\n--- Testing mlock on region1 ---\n");
    if (mlock(region1, PAGE_SIZE * NUM_PAGES) != 0) {
        perror("mlock region1");
        printf("WARNING: mlock failed (may need more privileges)\n");
        // Continue anyway - not fatal
    } else {
        printf("Successfully locked region1\n");
        
        // Verify data still accessible
        for (i = 0; i < NUM_PAGES; i++) {
            unsigned char *p = (unsigned char *)region1 + (i * PAGE_SIZE);
            if (*p != 0xAA) {
                printf("ERROR: region1 page %d corrupted after mlock\n", i);
                pass = 0;
            }
        }
        
        // Write new data to locked pages
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t *p = (uint32_t *)((char *)region1 + (i * PAGE_SIZE));
            *p = 0xDEAD0000 | i;
        }
        
        // Verify writes
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t *p = (uint32_t *)((char *)region1 + (i * PAGE_SIZE));
            if (*p != (uint32_t)(0xDEAD0000 | i)) {
                printf("ERROR: Write to locked page %d failed\n", i);
                pass = 0;
            }
        }
    }
    
    // Test 2: Allocate new region while first is locked
    printf("\n--- Allocating new region with locked memory ---\n");
    region3 = mmap(NULL, PAGE_SIZE * 8,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region3 == MAP_FAILED) {
        perror("mmap region3");
        pass = 0;
    } else {
        printf("Allocated region3 at %p\n", region3);
        memset(region3, 0xCC, PAGE_SIZE * 8);
        
        // Lock this too
        if (mlock(region3, PAGE_SIZE * 8) != 0) {
            printf("Could not lock region3\n");
        }
    }
    
    // Test 3: mlockall with replication
    printf("\n--- Testing mlockall ---\n");
    if (mlockall(MCL_CURRENT) != 0) {
        perror("mlockall(MCL_CURRENT)");
        printf("WARNING: mlockall failed (may need more privileges)\n");
    } else {
        printf("mlockall(MCL_CURRENT) succeeded\n");
        
        // Allocate more memory after mlockall
        void *post_lock = malloc(PAGE_SIZE);
        if (post_lock) {
            memset(post_lock, 0xDD, PAGE_SIZE);
            if (*(unsigned char *)post_lock != 0xDD) {
                printf("ERROR: Post-mlockall allocation corrupted\n");
                pass = 0;
            }
            free(post_lock);
        }
    }
    
    // Test 4: munlock with replication
    printf("\n--- Testing munlock ---\n");
    if (munlock(region1, PAGE_SIZE * NUM_PAGES) != 0) {
        perror("munlock region1");
    } else {
        printf("Successfully unlocked region1\n");
        
        // Verify data still intact after unlock
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t *p = (uint32_t *)((char *)region1 + (i * PAGE_SIZE));
            if (*p != (uint32_t)(0xDEAD0000 | i)) {
                printf("ERROR: region1 page %d corrupted after munlock\n", i);
                pass = 0;
            }
        }
    }
    
    // Test 5: Partial mlock
    printf("\n--- Testing partial mlock ---\n");
    if (mlock((char *)region2 + PAGE_SIZE * 4, PAGE_SIZE * 8) != 0) {
        perror("mlock partial");
    } else {
        printf("Locked middle 8 pages of region2\n");
        
        // Write pattern to verify boundaries
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t *p = (uint32_t *)((char *)region2 + (i * PAGE_SIZE));
            *p = 0xBEEF0000 | i;
        }
        
        // Verify all pages
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t *p = (uint32_t *)((char *)region2 + (i * PAGE_SIZE));
            if (*p != (uint32_t)(0xBEEF0000 | i)) {
                printf("ERROR: region2 page %d wrong after partial lock\n", i);
                pass = 0;
            }
        }
    }
    
    // Disable replication
    printf("\n--- Disabling replication ---\n");
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL disable)");
        pass = 0;
    }
    
    // Final verification after disable
    printf("Verifying all regions after disable...\n");
    
    // Check region1
    for (i = 0; i < NUM_PAGES; i++) {
        uint32_t *p = (uint32_t *)((char *)region1 + (i * PAGE_SIZE));
        if (*p != (uint32_t)(0xDEAD0000 | i)) {
            printf("ERROR: region1 page %d corrupted after disable\n", i);
            pass = 0;
        }
    }
    
    // Check region2
    for (i = 0; i < NUM_PAGES; i++) {
        uint32_t *p = (uint32_t *)((char *)region2 + (i * PAGE_SIZE));
        if (*p != (uint32_t)(0xBEEF0000 | i)) {
            printf("ERROR: region2 page %d corrupted after disable\n", i);
            pass = 0;
        }
    }
    
    // Cleanup
    munlockall();
    munmap(region1, PAGE_SIZE * NUM_PAGES);
    munmap(region2, PAGE_SIZE * NUM_PAGES);
    if (region3 != MAP_FAILED) {
        munmap(region3, PAGE_SIZE * 8);
    }
    
    if (pass) {
        printf("\n*** TEST 25 PASSED ***\n");
        printf("Memory locking works correctly with replication\n");
    } else {
        printf("\n*** TEST 25 FAILED ***\n");
        printf("Issues with memory locking under replication\n");
    }
    
    return pass ? 0 : 1;
}
