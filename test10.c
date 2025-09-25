#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define NUM_PAGES 10

int main(void) {
    long ret;
    pid_t pid;
    int status;
    char *mem;
    int i;
    
    printf("TEST10: Copy-on-Write (COW) After Fork Test\n");
    printf("============================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Allocate and initialize memory before fork
    mem = mmap(NULL, 4096 * NUM_PAGES, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        printf("FAIL: mmap failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Allocated %d pages\n", NUM_PAGES);
    
    // Initialize each page with unique pattern
    for (i = 0; i < NUM_PAGES; i++) {
        memset(mem + (i * 4096), 'A' + i, 4096);
        mem[i * 4096] = '0' + i;  // First byte as page number
    }
    printf("PASS: Initialized pages with patterns\n");
    
    // Fork with replication enabled
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed: %s\n", strerror(errno));
        munmap(mem, 4096 * NUM_PAGES);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    if (pid == 0) {
        // Child process
        // Verify child doesn't have replication
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret != 0) {
            printf("FAIL: Child has replication enabled\n");
            exit(1);
        }
        
        // Verify child can read parent's data (COW not triggered yet)
        for (i = 0; i < NUM_PAGES; i++) {
            if (mem[i * 4096] != '0' + i) {
                printf("FAIL: Child reads wrong data at page %d before COW\n", i);
                exit(1);
            }
        }
        printf("CHILD: Can read parent's data (COW pages shared)\n");
        
        // Modify even-numbered pages (trigger COW)
        for (i = 0; i < NUM_PAGES; i += 2) {
            mem[i * 4096] = 'C';  // C for Child
            memset(mem + (i * 4096) + 1, 'X', 100);  // Modify more bytes
        }
        printf("CHILD: Modified even pages (triggered COW)\n");
        
        // Verify modifications took effect
        for (i = 0; i < NUM_PAGES; i++) {
            if (i % 2 == 0) {
                if (mem[i * 4096] != 'C') {
                    printf("FAIL: Child's COW write didn't work on page %d\n", i);
                    exit(1);
                }
            } else {
                // Odd pages should still have original data
                if (mem[i * 4096] != '0' + i) {
                    printf("FAIL: Unmodified page %d corrupted\n", i);
                    exit(1);
                }
            }
        }
        printf("CHILD: COW pages successfully modified\n");
        exit(0);
    }
    
    // Parent process
    // Wait a bit to let child read memory first
    usleep(100000);  // 100ms
    
    // Parent modifies odd-numbered pages
    for (i = 1; i < NUM_PAGES; i += 2) {
        mem[i * 4096] = 'P';  // P for Parent
        memset(mem + (i * 4096) + 1, 'Z', 100);
    }
    printf("PARENT: Modified odd pages (triggered COW)\n");
    
    // Wait for child to complete
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: Child process failed\n");
        munmap(mem, 4096 * NUM_PAGES);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Child completed successfully\n");
    
    // Verify parent's pages are correct (child's modifications shouldn't affect parent)
    for (i = 0; i < NUM_PAGES; i++) {
        if (i % 2 == 0) {
            // Even pages should still have original data (child modified these)
            if (mem[i * 4096] != '0' + i) {
                printf("FAIL: Parent's even page %d was corrupted (got '%c')\n", 
                       i, mem[i * 4096]);
                munmap(mem, 4096 * NUM_PAGES);
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                return 1;
            }
        } else {
            // Odd pages should have parent's modifications
            if (mem[i * 4096] != 'P') {
                printf("FAIL: Parent's odd page %d modification lost\n", i);
                munmap(mem, 4096 * NUM_PAGES);
                prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
                return 1;
            }
        }
    }
    printf("PASS: COW isolation verified - parent and child had separate pages\n");
    
    // Verify replication still enabled in parent
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Parent's replication disabled after fork/COW\n");
        munmap(mem, 4096 * NUM_PAGES);
        return 1;
    }
    printf("PASS: Parent's replication still enabled (0x%lx)\n", ret);
    
    // Clean up
    munmap(mem, 4096 * NUM_PAGES);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST10: SUCCESS - COW works correctly with replication\n");
    return 0;
}
