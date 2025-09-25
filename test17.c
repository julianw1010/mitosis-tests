// test17.c - MAP_FIXED Overlapping Replicated Mappings (FIXED)
// Tests that MAP_FIXED properly cleans up overlapped replicated page tables

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <errno.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define MAP_SIZE (2 * 1024 * 1024)  // 2MB
#define PATTERN1 0xAA
#define PATTERN2 0xBB
#define PATTERN3 0xCC

int main(void) {
    unsigned char *ptr1, *ptr2, *ptr3;  // Changed to unsigned char
    int ret;
    int test_passed = 1;
    size_t i;
    
    printf("Test 17: MAP_FIXED Overlapping Replicated Mappings\n");
    printf("===================================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication\n");
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled on nodes: 0x%x\n", ret);
    
    // Create initial mapping
    ptr1 = mmap(NULL, MAP_SIZE * 2, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr1 == MAP_FAILED) {
        printf("FAIL: Initial mmap failed\n");
        return 1;
    }
    printf("Initial mapping at %p (size: %zu)\n", ptr1, MAP_SIZE * 2);
    
    // Fill with pattern and fault in pages
    printf("Writing initial pattern 0x%02x...\n", PATTERN1);
    for (i = 0; i < MAP_SIZE * 2; i++) {
        ptr1[i] = PATTERN1;
    }
    
    // Verify initial pattern
    printf("Verifying initial pattern...\n");
    for (i = 0; i < MAP_SIZE * 2; i++) {
        if (ptr1[i] != PATTERN1) {
            printf("FAIL: Initial pattern wrong at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, PATTERN1, ptr1[i]);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Initial mapping verified (%zu bytes)\n", MAP_SIZE * 2);
    
    // Now use MAP_FIXED to replace the middle portion
    printf("\nReplacing middle portion with MAP_FIXED...\n");
    printf("  Target address: %p\n", ptr1 + MAP_SIZE/2);
    printf("  Size: %zu bytes\n", MAP_SIZE);
    
    ptr2 = mmap(ptr1 + MAP_SIZE/2, MAP_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ptr2 == MAP_FAILED) {
        printf("FAIL: MAP_FIXED mmap failed: %s\n", strerror(errno));
        test_passed = 0;
        goto cleanup;
    }
    printf("MAP_FIXED mapping succeeded at %p\n", ptr2);
    
    // Fill new mapping with different pattern
    printf("Writing pattern 0x%02x to replaced region...\n", PATTERN2);
    for (i = 0; i < MAP_SIZE; i++) {
        ptr2[i] = PATTERN2;
    }
    
    // Verify memory layout:
    // First MAP_SIZE/2 bytes should still be PATTERN1
    // Next MAP_SIZE bytes should be PATTERN2
    // Last MAP_SIZE/2 bytes should be PATTERN1
    
    printf("\nVerifying memory layout after MAP_FIXED:\n");
    printf("  [0 - %zu): Should be 0x%02x\n", MAP_SIZE/2, PATTERN1);
    printf("  [%zu - %zu): Should be 0x%02x\n", MAP_SIZE/2, MAP_SIZE/2 + MAP_SIZE, PATTERN2);
    printf("  [%zu - %zu): Should be 0x%02x\n", MAP_SIZE/2 + MAP_SIZE, MAP_SIZE * 2, PATTERN1);
    
    // Check first part
    for (i = 0; i < MAP_SIZE/2; i++) {
        if (ptr1[i] != PATTERN1) {
            printf("FAIL: First part corrupted at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, PATTERN1, ptr1[i]);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("  First part: OK\n");
    
    // Check middle (replaced) part
    for (i = MAP_SIZE/2; i < MAP_SIZE/2 + MAP_SIZE; i++) {
        if (ptr1[i] != PATTERN2) {
            printf("FAIL: Middle part wrong at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, PATTERN2, ptr1[i]);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("  Middle part: OK\n");
    
    // Check last part
    for (i = MAP_SIZE/2 + MAP_SIZE; i < MAP_SIZE * 2; i++) {
        if (ptr1[i] != PATTERN1) {
            printf("FAIL: Last part corrupted at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, PATTERN1, ptr1[i]);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("  Last part: OK\n");
    printf("Memory layout correct after MAP_FIXED\n");
    
    // Do another MAP_FIXED that completely replaces everything
    printf("\nCompletely replacing with MAP_FIXED...\n");
    ptr3 = mmap(ptr1, MAP_SIZE * 2, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ptr3 == MAP_FAILED) {
        printf("FAIL: Complete MAP_FIXED failed: %s\n", strerror(errno));
        test_passed = 0;
        goto cleanup;
    }
    
    // Fill with new pattern
    printf("Writing pattern 0x%02x to entire region...\n", PATTERN3);
    for (i = 0; i < MAP_SIZE * 2; i++) {
        ptr3[i] = PATTERN3;
    }
    
    // Verify complete replacement
    for (i = 0; i < MAP_SIZE * 2; i++) {
        if (ptr3[i] != PATTERN3) {
            printf("FAIL: Complete replacement wrong at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, PATTERN3, ptr3[i]);
            test_passed = 0;
            goto cleanup;
        }
    }
    printf("Complete replacement verified\n");
    
cleanup:
    // Clean up any remaining mappings
    munmap(ptr1, MAP_SIZE * 2);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    if (test_passed) {
        printf("\n✓ Test 17 PASSED: MAP_FIXED correctly handles overlapping replicated mappings\n");
        return 0;
    } else {
        printf("\n✗ Test 17 FAILED: Issues with MAP_FIXED and replicated page tables\n");
        return 1;
    }
}
