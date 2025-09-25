// test21.c - Multiple processes with replication enabled simultaneously
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define NUM_PROCESSES 4
#define MEMORY_SIZE (1024 * 1024)  // 1MB per process
#define ITERATIONS 100

int worker_process(int id) {
    void *mem;
    int i, j;
    unsigned char pattern = (unsigned char)(id * 17);  // Unique pattern per process
    
    printf("Worker %d: Starting (PID %d)\n", id, getpid());
    
    // Allocate memory first
    mem = malloc(MEMORY_SIZE);
    if (!mem) {
        printf("Worker %d: Failed to allocate memory\n", id);
        return 1;
    }
    
    // Fill with initial pattern
    memset(mem, pattern, MEMORY_SIZE);
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        printf("Worker %d: Failed to enable replication: %s\n", id, strerror(errno));
        free(mem);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Worker %d: Replication enabled with mask 0x%lx\n", id, mask);
    
    // Do some memory operations
    for (i = 0; i < ITERATIONS; i++) {
        // Write pattern
        for (j = 0; j < MEMORY_SIZE; j += 4096) {
            ((unsigned char *)mem)[j] = pattern + i;
        }
        
        // Read and verify
        for (j = 0; j < MEMORY_SIZE; j += 4096) {
            if (((unsigned char *)mem)[j] != (unsigned char)(pattern + i)) {
                printf("Worker %d: Data corruption at offset %d, iteration %d\n", 
                       id, j, i);
                printf("  Expected: 0x%02x, Got: 0x%02x\n", 
                       (unsigned char)(pattern + i), ((unsigned char *)mem)[j]);
                free(mem);
                return 1;
            }
        }
        
        // Occasionally yield to increase concurrency
        if (i % 10 == 0) {
            usleep(1000);  // 1ms
        }
    }
    
    printf("Worker %d: Memory operations completed successfully\n", id);
    
    // Disable replication
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        printf("Worker %d: Failed to disable replication: %s\n", id, strerror(errno));
        free(mem);
        return 1;
    }
    
    // Final verification after disable
    for (j = 0; j < MEMORY_SIZE; j += 4096) {
        if (((unsigned char *)mem)[j] != (unsigned char)(pattern + ITERATIONS - 1)) {
            printf("Worker %d: Data corruption after disable at offset %d\n", id, j);
            free(mem);
            return 1;
        }
    }
    
    free(mem);
    printf("Worker %d: SUCCESS - Completed all operations\n", id);
    return 0;
}

int main(void) {
    pid_t pids[NUM_PROCESSES];
    int i, status;
    int pass = 1;
    
    printf("Test 21: Multiple Processes with Simultaneous Replication\n");
    printf("=========================================================\n");
    printf("Creating %d worker processes...\n", NUM_PROCESSES);
    
    // Fork worker processes
    for (i = 0; i < NUM_PROCESSES; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            
            // Kill any already-created workers
            for (int j = 0; j < i; j++) {
                kill(pids[j], SIGTERM);
            }
            return 1;
        }
        
        if (pids[i] == 0) {
            // Child process
            exit(worker_process(i));
        }
        
        printf("Created worker %d with PID %d\n", i, pids[i]);
        
        // Small delay between forks to avoid thundering herd
        usleep(10000);  // 10ms
    }
    
    printf("\nAll workers created, waiting for completion...\n");
    
    // Wait for all workers to complete
    for (i = 0; i < NUM_PROCESSES; i++) {
        pid_t pid = waitpid(pids[i], &status, 0);
        
        if (pid != pids[i]) {
            printf("ERROR: waitpid returned unexpected PID %d (expected %d)\n", 
                   pid, pids[i]);
            pass = 0;
            continue;
        }
        
        if (!WIFEXITED(status)) {
            printf("Worker %d (PID %d): Abnormal termination\n", i, pids[i]);
            pass = 0;
        } else if (WEXITSTATUS(status) != 0) {
            printf("Worker %d (PID %d): Failed with status %d\n", 
                   i, pids[i], WEXITSTATUS(status));
            pass = 0;
        } else {
            printf("Worker %d (PID %d): Completed successfully\n", i, pids[i]);
        }
    }
    
    if (pass) {
        printf("\n*** TEST 21 PASSED ***\n");
        printf("Multiple processes can use replication simultaneously without issues\n");
    } else {
        printf("\n*** TEST 21 FAILED ***\n");
        printf("Issues with concurrent replication across multiple processes\n");
    }
    
    return pass ? 0 : 1;
}
