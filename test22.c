// test22.c - Transparent huge pages and explicit huge pages with replication
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdint.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define HUGE_PAGE_SIZE (2 * 1024 * 1024)  // 2MB
#define NUM_HUGE_PAGES 4
#define TEST_PATTERN 0xDEADBEEF

int main(void) {
    void *thp_mem, *explicit_huge;
    uint32_t *ptr;
    int i, j;
    int pass = 1;
    
    printf("Test 22: Huge Pages (2MB) with Replication\n");
    printf("==========================================\n");
    
    // First test: Transparent Huge Pages (THP)
    printf("\n--- Testing Transparent Huge Pages ---\n");
    
    // Allocate aligned memory that could become a THP
    if (posix_memalign(&thp_mem, HUGE_PAGE_SIZE, HUGE_PAGE_SIZE * NUM_HUGE_PAGES) != 0) {
        perror("posix_memalign");
        return 1;
    }
    
    // Advise kernel to use huge pages
    if (madvise(thp_mem, HUGE_PAGE_SIZE * NUM_HUGE_PAGES, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE)");
        // Continue anyway - not fatal
    }
    
    printf("Allocated %d MB aligned for THP at %p\n", 
           (HUGE_PAGE_SIZE * NUM_HUGE_PAGES) / (1024*1024), thp_mem);
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        free(thp_mem);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled with mask: 0x%lx\n", mask);
    
    // Touch pages to trigger allocation (may become huge pages)
    ptr = (uint32_t *)thp_mem;
    for (i = 0; i < NUM_HUGE_PAGES; i++) {
        size_t offset = (i * HUGE_PAGE_SIZE) / sizeof(uint32_t);
        // Write to start of each potential huge page
        ptr[offset] = TEST_PATTERN + i;
        // Write to middle
        ptr[offset + 1024] = TEST_PATTERN + i + 0x1000;
        // Write to near end
        ptr[offset + (HUGE_PAGE_SIZE/sizeof(uint32_t)) - 1] = TEST_PATTERN + i + 0x2000;
    }
    
    printf("Wrote test patterns to THP memory\n");
    
    // Verify data
    for (i = 0; i < NUM_HUGE_PAGES; i++) {
        size_t offset = (i * HUGE_PAGE_SIZE) / sizeof(uint32_t);
        
        if (ptr[offset] != (uint32_t)(TEST_PATTERN + i)) {
            printf("ERROR: THP page %d start mismatch: got 0x%x, expected 0x%x\n",
                   i, ptr[offset], TEST_PATTERN + i);
            pass = 0;
        }
        
        if (ptr[offset + 1024] != (uint32_t)(TEST_PATTERN + i + 0x1000)) {
            printf("ERROR: THP page %d middle mismatch\n", i);
            pass = 0;
        }
        
        if (ptr[offset + (HUGE_PAGE_SIZE/sizeof(uint32_t)) - 1] != 
            (uint32_t)(TEST_PATTERN + i + 0x2000)) {
            printf("ERROR: THP page %d end mismatch\n", i);
            pass = 0;
        }
    }
    
    // Second test: Explicit huge pages (if available)
    printf("\n--- Testing Explicit Huge Pages ---\n");
    
    // Try to map explicit huge pages
    explicit_huge = mmap(NULL, HUGE_PAGE_SIZE * 2, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                        -1, 0);
    
    if (explicit_huge == MAP_FAILED) {
        printf("NOTE: Explicit huge pages not available (%s), skipping\n", 
               strerror(errno));
        // Not a failure - system might not have huge pages configured
    } else {
        printf("Mapped explicit huge pages at %p\n", explicit_huge);
        
        // Write pattern
        ptr = (uint32_t *)explicit_huge;
        for (j = 0; j < (HUGE_PAGE_SIZE * 2) / sizeof(uint32_t); j += 4096) {
            ptr[j] = TEST_PATTERN ^ j;
        }
        
        // Verify pattern
        for (j = 0; j < (HUGE_PAGE_SIZE * 2) / sizeof(uint32_t); j += 4096) {
            if (ptr[j] != (uint32_t)(TEST_PATTERN ^ j)) {
                printf("ERROR: Explicit huge page corruption at offset %d\n", j);
                pass = 0;
                break;
            }
        }
        
        if (munmap(explicit_huge, HUGE_PAGE_SIZE * 2) != 0) {
            perror("munmap explicit huge");
            pass = 0;
        }
    }
    
    // Test splitting/collapsing with mprotect
    printf("\n--- Testing huge page split/collapse ---\n");
    
    // Change protection on middle of potential huge page (forces split)
    void *middle = (char *)thp_mem + HUGE_PAGE_SIZE + 4096;
    if (mprotect(middle, 4096, PROT_READ) != 0) {
        perror("mprotect");
        pass = 0;
    } else {
        printf("Changed protection on middle page (may split huge page)\n");
        
        // Verify surrounding pages still writable
        ptr = (uint32_t *)thp_mem;
        ptr[0] = 0x12345678;
        ptr[(HUGE_PAGE_SIZE * 2 / sizeof(uint32_t)) - 1] = 0x87654321;
        
        if (ptr[0] != 0x12345678 || 
            ptr[(HUGE_PAGE_SIZE * 2 / sizeof(uint32_t)) - 1] != 0x87654321) {
            printf("ERROR: Data corruption after mprotect\n");
            pass = 0;
        }
        
        // Restore protection
        if (mprotect(middle, 4096, PROT_READ | PROT_WRITE) != 0) {
            perror("mprotect restore");
            pass = 0;
        }
    }
    
    // Disable replication
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL disable)");
        pass = 0;
    }
    
    // Final verification after disable
    ptr = (uint32_t *)thp_mem;
    volatile uint32_t test = ptr[0];
    test = ptr[HUGE_PAGE_SIZE / sizeof(uint32_t)];
    test = ptr[(HUGE_PAGE_SIZE * 2) / sizeof(uint32_t)];
    (void)test;
    
    printf("Memory still accessible after disable\n");
    
    free(thp_mem);
    
    if (pass) {
        printf("\n*** TEST 22 PASSED ***\n");
        printf("Huge pages work correctly with replication\n");
    } else {
        printf("\n*** TEST 22 FAILED ***\n");
        printf("Issues with huge pages under replication\n");
    }
    
    return pass ? 0 : 1;
}
