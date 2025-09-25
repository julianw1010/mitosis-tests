// test29.c - File-backed memory mapping test
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    int ret;
    int fd;
    char *filename = "/tmp/mitosis_test29.dat";
    size_t file_size = 16 * 4096; // 64KB
    void *map_private, *map_shared;
    
    // Check NUMA availability
    if (numa_available() < 0) {
        printf("SKIP: NUMA not available\n");
        return 0;
    }
    
    if (numa_num_configured_nodes() < 2) {
        printf("SKIP: Need at least 2 NUMA nodes\n");
        return 0;
    }
    
    // Create test file
    fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("FAIL: Could not create test file: %s\n", strerror(errno));
        return 1;
    }
    
    // Write initial data
    char *initial_data = malloc(file_size);
    if (!initial_data) {
        printf("FAIL: Could not allocate initial data\n");
        close(fd);
        unlink(filename);
        return 1;
    }
    
    for (size_t i = 0; i < file_size; i++) {
        initial_data[i] = (char)(i & 0xFF);
    }
    
    if (write(fd, initial_data, file_size) != file_size) {
        printf("FAIL: Could not write to file: %s\n", strerror(errno));
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Verify enabled
    unsigned long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication not enabled\n");
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Test 1: MAP_PRIVATE mapping (copy-on-write)
    printf("Testing MAP_PRIVATE file mapping...\n");
    map_private = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE, fd, 0);
    if (map_private == MAP_FAILED) {
        printf("FAIL: MAP_PRIVATE mmap failed: %s\n", strerror(errno));
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Verify initial data
    unsigned char *priv_buf = (unsigned char*)map_private;
    for (size_t i = 0; i < file_size; i += 4096) {
        if (priv_buf[i] != (unsigned char)(i & 0xFF)) {
            printf("FAIL: Initial data mismatch at offset %zu\n", i);
            munmap(map_private, file_size);
            free(initial_data);
            close(fd);
            unlink(filename);
            return 1;
        }
    }
    
    // Modify private mapping (triggers COW)
    printf("Modifying private mapping (COW)...\n");
    for (size_t i = 0; i < file_size; i += 4096) {
        priv_buf[i] = 0xAA;
    }
    
    // Verify modifications are visible
    for (size_t i = 0; i < file_size; i += 4096) {
        if (priv_buf[i] != 0xAA) {
            printf("FAIL: Private mapping modification failed at offset %zu\n", i);
            munmap(map_private, file_size);
            free(initial_data);
            close(fd);
            unlink(filename);
            return 1;
        }
    }
    
    // Test 2: MAP_SHARED mapping
    printf("Testing MAP_SHARED file mapping...\n");
    map_shared = mmap(NULL, file_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (map_shared == MAP_FAILED) {
        printf("FAIL: MAP_SHARED mmap failed: %s\n", strerror(errno));
        munmap(map_private, file_size);
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Shared mapping should see original file data (not private modifications)
    unsigned char *shared_buf = (unsigned char*)map_shared;
    for (size_t i = 0; i < file_size; i += 4096) {
        if (shared_buf[i] != (unsigned char)(i & 0xFF)) {
            printf("FAIL: Shared mapping doesn't see original data at offset %zu\n", i);
            munmap(map_private, file_size);
            munmap(map_shared, file_size);
            free(initial_data);
            close(fd);
            unlink(filename);
            return 1;
        }
    }
    
    // Modify shared mapping (affects file)
    printf("Modifying shared mapping...\n");
    for (size_t i = 0; i < file_size; i += 8192) {
        shared_buf[i] = 0xBB;
    }
    
    // Sync to file
    if (msync(map_shared, file_size, MS_SYNC) < 0) {
        printf("WARNING: msync failed: %s\n", strerror(errno));
    }
    
    // Test 3: Create new mapping after modifications
    printf("Creating new mapping to verify persistence...\n");
    void *map_verify = mmap(NULL, file_size, PROT_READ,
                           MAP_PRIVATE, fd, 0);
    if (map_verify == MAP_FAILED) {
        printf("FAIL: Verification mmap failed: %s\n", strerror(errno));
        munmap(map_private, file_size);
        munmap(map_shared, file_size);
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // New mapping should see shared modifications
    unsigned char *verify_buf = (unsigned char*)map_verify;
    for (size_t i = 0; i < file_size; i += 8192) {
        if (verify_buf[i] != 0xBB) {
            printf("FAIL: Shared modifications not visible in new mapping at offset %zu\n", i);
            munmap(map_private, file_size);
            munmap(map_shared, file_size);
            munmap(map_verify, file_size);
            free(initial_data);
            close(fd);
            unlink(filename);
            return 1;
        }
    }
    
    // Verify replication still active
    status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status == 0) {
        printf("FAIL: Replication disabled during file mapping operations\n");
        munmap(map_private, file_size);
        munmap(map_shared, file_size);
        munmap(map_verify, file_size);
        free(initial_data);
        close(fd);
        unlink(filename);
        return 1;
    }
    
    // Cleanup
    munmap(map_private, file_size);
    munmap(map_shared, file_size);
    munmap(map_verify, file_size);
    free(initial_data);
    close(fd);
    unlink(filename);
    
    // Disable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret < 0) {
        printf("WARNING: Could not disable replication\n");
    }
    
    printf("PASS: File-backed memory mapping test completed successfully\n");
    return 0;
}
