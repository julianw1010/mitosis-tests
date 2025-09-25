#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define NUM_CYCLES 5
#define NUM_ALLOCATIONS 10

int main(void) {
    long ret;
    void *allocations[NUM_ALLOCATIONS];
    int cycle, i;
    char pattern;
    
    printf("TEST11: Multiple Enable/Disable Cycles Test\n");
    printf("============================================\n");
    
    for (cycle = 0; cycle < NUM_CYCLES; cycle++) {
        printf("\n--- Cycle %d/%d ---\n", cycle + 1, NUM_CYCLES);
        
        // Enable replication
        ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Could not enable replication on cycle %d: %s\n", 
                   cycle + 1, strerror(errno));
            return 1;
        }
        
        // Verify it's enabled
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret <= 0) {
            printf("FAIL: Replication not enabled on cycle %d\n", cycle + 1);
            return 1;
        }
        printf("PASS: Enabled replication (mask=0x%lx)\n", ret);
        
        // Allocate memory with replication active
        for (i = 0; i < NUM_ALLOCATIONS; i++) {
            allocations[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (allocations[i] == MAP_FAILED) {
                printf("FAIL: mmap failed on cycle %d, alloc %d: %s\n",
                       cycle + 1, i, strerror(errno));
                // Clean up previous allocations
                while (--i >= 0) {
                    munmap(allocations[i], 4096);
                }
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                return 1;
            }
            
            // Write unique pattern
            pattern = (cycle * NUM_ALLOCATIONS + i) % 256;
            memset(allocations[i], pattern, 4096);
        }
        printf("PASS: Allocated and wrote %d pages\n", NUM_ALLOCATIONS);
        
        // Verify all patterns are correct
        for (i = 0; i < NUM_ALLOCATIONS; i++) {
            pattern = (cycle * NUM_ALLOCATIONS + i) % 256;
            if (((char *)allocations[i])[0] != pattern ||
                ((char *)allocations[i])[2048] != pattern ||
                ((char *)allocations[i])[4095] != pattern) {
                printf("FAIL: Pattern verification failed on cycle %d, page %d\n",
                       cycle + 1, i);
                for (i = 0; i < NUM_ALLOCATIONS; i++) {
                    munmap(allocations[i], 4096);
                }
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                return 1;
            }
        }
        printf("PASS: All patterns verified correctly\n");
        
        // Disable replication
        ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Could not disable replication on cycle %d: %s\n",
                   cycle + 1, strerror(errno));
            for (i = 0; i < NUM_ALLOCATIONS; i++) {
                munmap(allocations[i], 4096);
            }
            return 1;
        }
        
        // Verify it's disabled
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret != 0) {
            printf("FAIL: Replication not disabled on cycle %d (ret=%ld)\n", 
                   cycle + 1, ret);
            for (i = 0; i < NUM_ALLOCATIONS; i++) {
                munmap(allocations[i], 4096);
            }
            return 1;
        }
        printf("PASS: Disabled replication\n");
        
        // Verify memory is still accessible after disable
        for (i = 0; i < NUM_ALLOCATIONS; i++) {
            pattern = (cycle * NUM_ALLOCATIONS + i) % 256;
            if (((char *)allocations[i])[0] != pattern) {
                printf("FAIL: Memory corrupted after disable on cycle %d\n", cycle + 1);
                for (i = 0; i < NUM_ALLOCATIONS; i++) {
                    munmap(allocations[i], 4096);
                }
                return 1;
            }
            // Write new pattern with replication disabled
            memset(allocations[i], ~pattern, 4096);
            if (((char *)allocations[i])[0] != (char)(~pattern)) {
                printf("FAIL: Cannot write after disable on cycle %d\n", cycle + 1);
                for (i = 0; i < NUM_ALLOCATIONS; i++) {
                    munmap(allocations[i], 4096);
                }
                return 1;
            }
        }
        printf("PASS: Memory remains accessible after disable\n");
        
        // Free all allocations
        for (i = 0; i < NUM_ALLOCATIONS; i++) {
            munmap(allocations[i], 4096);
        }
        printf("PASS: Freed all allocations\n");
        
        // Small delay between cycles
        usleep(10000);  // 10ms
    }
    
    // Final verification - should still be able to enable after all cycles
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Cannot enable after %d cycles: %s\n", 
               NUM_CYCLES, strerror(errno));
        return 1;
    }
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Final enable failed\n");
        return 1;
    }
    printf("\nPASS: Can still enable after %d cycles (mask=0x%lx)\n", 
           NUM_CYCLES, ret);
    
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST11: SUCCESS - Multiple enable/disable cycles work correctly\n");
    return 0;
}
