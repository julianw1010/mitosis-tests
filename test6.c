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

int main(int argc, char *argv[]) {
    long ret;
    pid_t pid;
    int status;
    
    // Check if we're the exec'd child
    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        // We are the exec'd process - check replication state
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret != 0) {
            printf("FAIL: Exec'd process has replication enabled (0x%lx), should be 0\n", ret);
            return 1;
        }
        printf("PASS: Exec'd process has clean state (replication=0)\n");
        
        // Verify we can enable independently
        ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
        if (ret < 0) {
            printf("FAIL: Exec'd process cannot enable replication: %s\n", strerror(errno));
            return 1;
        }
        
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret <= 0) {
            printf("FAIL: Exec'd process enable failed\n");
            return 1;
        }
        printf("PASS: Exec'd process can enable replication independently (0x%lx)\n", ret);
        return 0;
    }
    
    printf("TEST6: Exec Transition Test\n");
    printf("============================\n");
    
    // Enable replication in parent
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Parent replication not enabled\n");
        return 1;
    }
    printf("PASS: Parent has replication enabled (0x%lx)\n", ret);
    
    // Allocate and write some memory before fork
    char *mem = malloc(4096);
    if (!mem) {
        printf("FAIL: malloc failed\n");
        return 1;
    }
    strcpy(mem, "TestData");
    printf("INFO: Parent allocated memory and wrote data\n");
    
    // Fork
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork() failed: %s\n", strerror(errno));
        free(mem);
        return 1;
    }
    
    if (pid == 0) {
        // Child process - exec ourselves with --child flag
        char *prog = argv[0];
        char *args[] = {prog, "--child", NULL};
        
        printf("INFO: Child about to exec...\n");
        execvp(prog, args);
        
        // If we get here, exec failed
        printf("FAIL: exec failed: %s\n", strerror(errno));
        exit(1);
    }
    
    // Parent waits for child
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: Child exec test failed\n");
        free(mem);
        return 1;
    }
    printf("PASS: Child successfully exec'd and verified state\n");
    
    // Verify parent still has replication after fork/exec
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Parent lost replication after fork/exec\n");
        free(mem);
        return 1;
    }
    printf("PASS: Parent still has replication after fork/exec (0x%lx)\n", ret);
    
    // Verify parent's memory is still intact
    if (strcmp(mem, "TestData") != 0) {
        printf("FAIL: Parent memory corrupted\n");
        free(mem);
        return 1;
    }
    printf("PASS: Parent memory intact after fork/exec\n");
    
    // Clean up
    free(mem);
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST6: SUCCESS - Exec transition works correctly\n");
    return 0;
}
