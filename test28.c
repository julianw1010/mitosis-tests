// test28.c - Different memory region types test (stack, heap, anonymous mmap)
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <alloca.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int test_stack_memory(void) {
    // Allocate on stack
    char stack_buffer[8192];
    
    // Write pattern
    memset(stack_buffer, 0xAA, sizeof(stack_buffer));
    
    // Verify
    for (int i = 0; i < sizeof(stack_buffer); i += 1024) {
        if (stack_buffer[i] != (char)0xAA) {
            printf("FAIL: Stack memory verification failed at offset %d\n", i);
            return 1;
        }
    }
    
    // Use alloca for dynamic stack allocation
    char *dynamic_stack = alloca(4096);
    memset(dynamic_stack, 0xBB, 4096);
    if (dynamic_stack[0] != (char)0xBB || dynamic_stack[4095] != (char)0xBB) {
        printf("FAIL: Dynamic stack (alloca) verification failed\n");
        return 1;
    }
    
    return 0;
}

int test_heap_memory(void) {
    // Test various heap sizes
    size_t sizes[] = {64, 1024, 4096, 65536, 1048576};
    
    for (int i = 0; i < 5; i++) {
        void *ptr = malloc(sizes[i]);
        if (!ptr) {
            printf("FAIL: Heap allocation of size %zu failed\n", sizes[i]);
            return 1;
        }
        
        // Write pattern
        memset(ptr, 0xCC + i, sizes[i]);
        
        // Verify
        unsigned char *buf = (unsigned char*)ptr;
        if (buf[0] != (0xCC + i) || buf[sizes[i]-1] != (0xCC + i)) {
            printf("FAIL: Heap memory verification failed for size %zu\n", sizes[i]);
            free(ptr);
            return 1;
        }
        
        free(ptr);
    }
    
    // Test calloc (zeroed memory)
    int *zeroed = calloc(1024, sizeof(int));
    if (!zeroed) {
        printf("FAIL: Calloc failed\n");
        return 1;
    }
    
    for (int i = 0; i < 1024; i++) {
        if (zeroed[i] != 0) {
            printf("FAIL: Calloc memory not zeroed at index %d\n", i);
            free(zeroed);
            return 1;
        }
        zeroed[i] = i;
    }
    
    for (int i = 0; i < 1024; i++) {
        if (zeroed[i] != i) {
            printf("FAIL: Calloc memory verification failed\n");
            free(zeroed);
            return 1;
        }
    }
    
    free(zeroed);
    return 0;
}

int test_mmap_memory(void) {
    size_t map_size = 16 * 4096; // 64KB
    
    // Anonymous private mapping
    void *map1 = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map1 == MAP_FAILED) {
        printf("FAIL: Anonymous mmap failed: %s\n", strerror(errno));
        return 1;
    }
    
    // Write pattern
    memset(map1, 0xDD, map_size);
    
    // Verify
    unsigned char *buf = (unsigned char*)map1;
    for (size_t i = 0; i < map_size; i += 4096) {
        if (buf[i] != 0xDD) {
            printf("FAIL: Mmap verification failed at offset %zu\n", i);
            munmap(map1, map_size);
            return 1;
        }
    }
    
    // Test mmap with MAP_POPULATE
    void *map2 = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (map2 == MAP_FAILED) {
        printf("WARNING: MAP_POPULATE mmap failed (non-critical)\n");
    } else {
        memset(map2, 0xEE, map_size);
        buf = (unsigned char*)map2;
        if (buf[0] != 0xEE || buf[map_size-1] != 0xEE) {
            printf("FAIL: MAP_POPULATE mmap verification failed\n");
            munmap(map1, map_size);
            munmap(map2, map_size);
            return 1;
        }
        munmap(map2, map_size);
    }
    
    munmap(map1, map_size);
    return 0;
}

int main(void) {
    int ret;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    if (numa_num_configured_nodes() < 2) {
        printf("SKIP: Need at least 2 NUMA nodes\n");
        return 0;
    }
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    // Verify enabled
    unsigned long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication not enabled\n");
        return 1;
    }
    
    // Test stack memory
    printf("Testing stack memory...\n");
    if (test_stack_memory() != 0) {
        printf("FAIL: Stack memory test failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Test heap memory
    printf("Testing heap memory...\n");
    if (test_heap_memory() != 0) {
        printf("FAIL: Heap memory test failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Test mmap memory
    printf("Testing mmap memory...\n");
    if (test_mmap_memory() != 0) {
        printf("FAIL: Mmap memory test failed\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Verify replication still enabled
    status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication disabled during tests\n");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("WARNING: Could not disable replication\n");
    }
    
    printf("PASS: Different memory region types test completed successfully\n");
    return 0;
}
