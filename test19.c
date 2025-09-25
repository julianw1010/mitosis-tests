// test19.c - Private file-backed memory with COW modifications under replication
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define TEST_FILE "/tmp/mitosis_test19.dat"
#define FILE_SIZE (4096 * 10)  // 10 pages

int main(void) {
    printf("Test 19: Private File Mappings with Modifications\n");
    printf("================================================\n");
    
    // Create test file
    int fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    // Write initial pattern to file
    unsigned char *pattern = malloc(FILE_SIZE);
    for (int i = 0; i < FILE_SIZE; i++) {
        pattern[i] = i % 256;
    }
    if (write(fd, pattern, FILE_SIZE) != FILE_SIZE) {
        perror("write");
        close(fd);
        free(pattern);
        return 1;
    }
    free(pattern);
    
    // Map file privately
    void *map = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE, 
                     MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    
    printf("Mapped file at %p\n", map);
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        munmap(map, FILE_SIZE);
        close(fd);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled with mask: 0x%lx\n", mask);
    
    // Modify some pages (triggers COW)
    unsigned char *data = (unsigned char *)map;
    
    // Modify first page
    printf("Modifying first page (COW should occur)...\n");
    data[0] = 0xAA;
    data[100] = 0xBB;
    
    // Modify middle page
    printf("Modifying middle page...\n");
    data[FILE_SIZE/2] = 0xCC;
    data[FILE_SIZE/2 + 1] = 0xDD;
    
    // Modify last page
    printf("Modifying last page...\n");
    data[FILE_SIZE - 1] = 0xEE;
    
    // Fork to verify modifications are private
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(map, FILE_SIZE);
        close(fd);
        return 1;
    }
    
    if (pid == 0) {
        // Child: make different modifications
        data[0] = 0xFF;
        data[FILE_SIZE/2] = 0x11;
        
        // Verify child sees its own modifications
        if (data[0] != 0xFF || data[FILE_SIZE/2] != 0x11) {
            printf("Child: ERROR - Modifications not visible\n");
            exit(1);
        }
        
        printf("Child: Sees own modifications correctly\n");
        exit(0);
    }
    
    // Parent: wait and verify
    int status;
    wait(&status);
    
    // Parent should still see original modifications
    int pass = 1;
    
    if (data[0] != 0xAA) {
        printf("ERROR: First page modification lost (got 0x%02X, expected 0xAA)\n", data[0]);
        pass = 0;
    }
    
    if (data[100] != 0xBB) {
        printf("ERROR: First page second mod lost (got 0x%02X, expected 0xBB)\n", data[100]);
        pass = 0;
    }
    
    if (data[FILE_SIZE/2] != 0xCC) {
        printf("ERROR: Middle page modification lost (got 0x%02X, expected 0xCC)\n", data[FILE_SIZE/2]);
        pass = 0;
    }
    
    if (data[FILE_SIZE - 1] != 0xEE) {
        printf("ERROR: Last page modification lost (got 0x%02X, expected 0xEE)\n", data[FILE_SIZE - 1]);
        pass = 0;
    }
    
    // Disable replication
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL disable)");
        pass = 0;
    }
    
    // Verify data still accessible after disable
    volatile unsigned char test = data[0];
    test = data[FILE_SIZE/2];
    test = data[FILE_SIZE - 1];
    (void)test; // Avoid unused warning
    
    // Cleanup
    munmap(map, FILE_SIZE);
    close(fd);
    unlink(TEST_FILE);
    
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("Child process failed\n");
        pass = 0;
    }
    
    if (pass) {
        printf("\n*** TEST 19 PASSED ***\n");
        printf("Private file mappings with COW work correctly with replication\n");
    } else {
        printf("\n*** TEST 19 FAILED ***\n");
        printf("Issues with private file mappings under replication\n");
    }
    
    return pass ? 0 : 1;
}
