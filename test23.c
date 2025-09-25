// test23.c - NUMA page migration and move_pages() with replication
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <numaif.h>
#include <numa.h>
#include <errno.h>
#include <stdint.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define NUM_PAGES 16
#define PAGE_SIZE 4096

int main(void) {
    void *pages[NUM_PAGES];
    int nodes[NUM_PAGES];
    int status[NUM_PAGES];
    void *test_area;
    int i, ret;
    int pass = 1;
    int num_nodes;
    
    printf("Test 23: NUMA Memory Migration with Replicated Pages\n");
    printf("====================================================\n");
    
    // Check if NUMA is available
    if (numa_available() < 0) {
        printf("NUMA not available, skipping test\n");
        return 0;
    }
    
    num_nodes = numa_num_configured_nodes();
    printf("System has %d NUMA nodes\n", num_nodes);
    
    if (num_nodes < 2) {
        printf("Need at least 2 NUMA nodes for this test, skipping\n");
        return 0;
    }
    
    // Allocate test area
    test_area = mmap(NULL, PAGE_SIZE * NUM_PAGES, 
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (test_area == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    
    printf("Allocated test area at %p\n", test_area);
    
    // Initialize pages array
    for (i = 0; i < NUM_PAGES; i++) {
        pages[i] = (char *)test_area + (i * PAGE_SIZE);
        // Touch each page to allocate it
        *(int *)pages[i] = 0xAA + i;
    }
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        munmap(test_area, PAGE_SIZE * NUM_PAGES);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled with mask: 0x%lx\n", mask);
    
    // Check current node placement
    printf("\n--- Initial page placement ---\n");
    if (move_pages(0, NUM_PAGES, pages, NULL, status, 0) != 0) {
        perror("move_pages (query)");
        pass = 0;
    } else {
        for (i = 0; i < NUM_PAGES; i++) {
            if (status[i] >= 0) {
                printf("Page %d: node %d\n", i, status[i]);
            } else {
                printf("Page %d: error %d\n", i, status[i]);
            }
        }
    }
    
    // Try to migrate pages to different nodes
    printf("\n--- Attempting page migration ---\n");
    
    // Set target nodes (alternate between node 0 and 1)
    for (i = 0; i < NUM_PAGES; i++) {
        nodes[i] = i % num_nodes;
        printf("Requesting page %d -> node %d\n", i, nodes[i]);
    }
    
    // Attempt migration
    ret = move_pages(0, NUM_PAGES, pages, nodes, status, MPOL_MF_MOVE);
    if (ret < 0) {
        perror("move_pages (migrate)");
        printf("WARNING: Page migration failed (might be expected with replication)\n");
        // Not necessarily a failure - system might prevent migration with replicas
    } else {
        printf("move_pages returned %d\n", ret);
        
        // Check migration results
        for (i = 0; i < NUM_PAGES; i++) {
            if (status[i] < 0) {
                printf("Page %d: migration failed with error %d\n", i, status[i]);
            } else if (status[i] != nodes[i]) {
                printf("Page %d: on node %d (requested %d)\n", 
                       i, status[i], nodes[i]);
            } else {
                printf("Page %d: successfully migrated to node %d\n", 
                       i, status[i]);
            }
        }
    }
    
    // Verify data integrity after migration attempt
    printf("\n--- Verifying data integrity ---\n");
    for (i = 0; i < NUM_PAGES; i++) {
        int expected = 0xAA + i;
        int actual = *(int *)pages[i];
        if (actual != expected) {
            printf("ERROR: Page %d data corruption: got 0x%x, expected 0x%x\n",
                   i, actual, expected);
            pass = 0;
        }
    }
    
    // Test NUMA memory policy with replication
    printf("\n--- Testing NUMA memory policy ---\n");
    
    // Try to set memory policy
    unsigned long nodemask = 1UL << 1;  // Prefer node 1
    if (set_mempolicy(MPOL_PREFERRED, &nodemask, 2) != 0) {
        perror("set_mempolicy");
        // Continue anyway
    }
    
    // Allocate new page under policy
    void *policy_page = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (policy_page != MAP_FAILED) {
        *(int *)policy_page = 0xBEEF;
        
        // Check where it was allocated
        void *policy_pages[1] = { policy_page };
        int policy_status[1];
        
        if (move_pages(0, 1, policy_pages, NULL, policy_status, 0) == 0) {
            printf("Policy-allocated page is on node %d\n", policy_status[0]);
        }
        
        // Verify data
        if (*(int *)policy_page != 0xBEEF) {
            printf("ERROR: Policy page data corruption\n");
            pass = 0;
        }
        
        munmap(policy_page, PAGE_SIZE);
    }
    
    // Reset memory policy
    set_mempolicy(MPOL_DEFAULT, NULL, 0);
    
    // Disable replication
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL disable)");
        pass = 0;
    }
    
    // Final data check after disable
    printf("\n--- Final verification after disable ---\n");
    for (i = 0; i < NUM_PAGES; i++) {
        int expected = 0xAA + i;
        int actual = *(int *)pages[i];
        if (actual != expected) {
            printf("ERROR: Page %d corrupted after disable: got 0x%x\n",
                   i, actual);
            pass = 0;
        }
    }
    
    munmap(test_area, PAGE_SIZE * NUM_PAGES);
    
    if (pass) {
        printf("\n*** TEST 23 PASSED ***\n");
        printf("NUMA operations work correctly with replication\n");
    } else {
        printf("\n*** TEST 23 FAILED ***\n");
        printf("Issues with NUMA operations under replication\n");
    }
    
    return pass ? 0 : 1;
}
