// test20.c - vfork() behavior with page table replication
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define TEST_SIZE 4096

volatile int parent_var = 0x1234;
volatile int child_touched = 0;

int main(void) {
    printf("Test 20: vfork() with Replicated Page Tables\n");
    printf("=============================================\n");
    
    // Allocate test memory
    int *heap_data = malloc(TEST_SIZE);
    if (!heap_data) {
        perror("malloc");
        return 1;
    }
    memset(heap_data, 0xAA, TEST_SIZE);
    heap_data[0] = 0xDEADBEEF;
    
    // Enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        free(heap_data);
        return 1;
    }
    
    unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    printf("Replication enabled with mask: 0x%lx\n", mask);
    printf("Parent var before vfork: 0x%x\n", parent_var);
    printf("Heap data before vfork: 0x%x\n", heap_data[0]);
    
    // vfork - child shares parent's memory space temporarily
    pid_t pid = vfork();
    if (pid < 0) {
        perror("vfork");
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        free(heap_data);
        return 1;
    }
    
    if (pid == 0) {
        // Child in vfork - shares parent's address space!
        // This is extremely dangerous with replication
        
        // Child can see parent's data
        if (parent_var != 0x1234) {
            _exit(1);  // Use _exit in vfork child
        }
        
        if (heap_data[0] != 0xDEADBEEF) {
            _exit(2);
        }
        
        // Modify shared variable (visible to parent)
        child_touched = 1;
        
        // Child must either exec or exit
        // We'll just exit
        _exit(0);
    }
    
    // Parent continues after child exits/execs
    int status;
    waitpid(pid, &status, 0);
    
    int pass = 1;
    
    // Check child's exit status
    if (!WIFEXITED(status)) {
        printf("ERROR: Child didn't exit normally\n");
        pass = 0;
    } else if (WEXITSTATUS(status) != 0) {
        printf("ERROR: Child failed with status %d\n", WEXITSTATUS(status));
        pass = 0;
    }
    
    // Verify child's modification is visible (vfork shares memory)
    if (child_touched != 1) {
        printf("ERROR: Child's modification not visible (got %d, expected 1)\n", 
               child_touched);
        pass = 0;
    } else {
        printf("Child's modification correctly visible: %d\n", child_touched);
    }
    
    // Verify parent data unchanged
    if (parent_var != 0x1234) {
        printf("ERROR: Parent var corrupted (got 0x%x, expected 0x1234)\n", 
               parent_var);
        pass = 0;
    }
    
    if (heap_data[0] != 0xDEADBEEF) {
        printf("ERROR: Heap data corrupted (got 0x%x, expected 0xDEADBEEF)\n", 
               heap_data[0]);
        pass = 0;
    }
    
    // Try another vfork that does exec
    pid = vfork();
    if (pid < 0) {
        perror("vfork 2");
        pass = 0;
    } else if (pid == 0) {
        // Child: exec something simple
        execl("/bin/true", "true", NULL);
        _exit(127);  // exec failed
    } else {
        // Parent: wait for exec
        waitpid(pid, &status, 0);
        
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (WEXITSTATUS(status) == 127) {
                printf("WARNING: exec failed (might be expected in test env)\n");
            } else {
                printf("ERROR: Second vfork child failed\n");
                pass = 0;
            }
        }
    }
    
    // Disable replication
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL disable)");
        pass = 0;
    }
    
    // Verify memory still accessible
    volatile int test = parent_var;
    test = heap_data[0];
    (void)test;
    
    free(heap_data);
    
    if (pass) {
        printf("\n*** TEST 20 PASSED ***\n");
        printf("vfork() works correctly with page table replication\n");
    } else {
        printf("\n*** TEST 20 FAILED ***\n");
        printf("Issues with vfork() under replication\n");
    }
    
    return pass ? 0 : 1;
}
