#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    pid_t pid;
    int status;
    
    printf("TEST2: Fork Inheritance Test - Child should NOT inherit replication\n");
    printf("====================================================================\n");
    
    // Enable replication in parent
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication in parent: %s\n", strerror(errno));
        return 1;
    }
    
    // Verify parent has replication enabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Parent should have replication enabled\n");
        return 1;
    }
    printf("PASS: Parent has replication enabled (bitmask=0x%lx)\n", ret);
    
    // Fork
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork() failed: %s\n", strerror(errno));
        return 1;
    }
    
    if (pid == 0) {
        // Child process
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret != 0) {
            printf("FAIL: Child inherited replication (bitmask=0x%lx), should be 0\n", ret);
            exit(1);
        }
        printf("PASS: Child does NOT have replication (0)\n");
        
        // Try to enable in child - should work independently
        ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Child cannot enable its own replication: %s\n", strerror(errno));
            exit(1);
        }
        
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret <= 0) {
            printf("FAIL: Child's independent enable failed\n");
            exit(1);
        }
        printf("PASS: Child can independently enable replication (bitmask=0x%lx)\n", ret);
        exit(0);
    }
    
    // Parent waits for child
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: Child process failed\n");
        return 1;
    }
    
    // Verify parent still has replication after fork
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Parent lost replication after fork\n");
        return 1;
    }
    printf("PASS: Parent still has replication after fork (bitmask=0x%lx)\n", ret);
    
    // Clean up - disable in parent
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST2: SUCCESS - Fork inheritance works correctly\n");
    return 0;
}
