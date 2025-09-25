#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

int main(void) {
    long ret;
    void *shared_mem;
    pid_t pid;
    int status;
    int *shared_counter;
    char *shared_buffer;
    
    printf("TEST8: Shared Memory with Replication Test\n");
    printf("===========================================\n");
    
    // Enable replication
    ret = prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);
    if (ret < 0) {
        printf("FAIL: Could not enable replication: %s\n", strerror(errno));
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // Test 1: Anonymous shared memory (MAP_SHARED)
    shared_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared_mem == MAP_FAILED) {
        printf("FAIL: mmap MAP_SHARED failed: %s\n", strerror(errno));
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Created anonymous shared memory\n");
    
    shared_counter = (int *)shared_mem;
    shared_buffer = (char *)(shared_mem + sizeof(int));
    *shared_counter = 0;
    strcpy(shared_buffer, "Parent");
    
    // Fork to test shared memory between processes
    pid = fork();
    if (pid < 0) {
        printf("FAIL: fork failed: %s\n", strerror(errno));
        munmap(shared_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    if (pid == 0) {
        // Child process
        // Child should NOT have replication, but shared memory should work
        ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        if (ret != 0) {
            printf("FAIL: Child has replication (should be disabled)\n");
            exit(1);
        }
        
        // Increment shared counter
        (*shared_counter)++;
        
        // Modify shared buffer
        strcat(shared_buffer, "+Child");
        
        // Read parent's value
        if (*shared_counter != 1) {
            printf("FAIL: Child sees wrong counter value: %d\n", *shared_counter);
            exit(1);
        }
        printf("CHILD: Modified shared memory (counter=%d, buffer='%s')\n",
               *shared_counter, shared_buffer);
        exit(0);
    }
    
    // Parent waits for child
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: Child process failed\n");
        munmap(shared_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    
    // Parent checks shared memory modifications
    if (*shared_counter != 1) {
        printf("FAIL: Parent sees wrong counter: %d\n", *shared_counter);
        munmap(shared_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    if (strcmp(shared_buffer, "Parent+Child") != 0) {
        printf("FAIL: Parent sees wrong buffer: '%s'\n", shared_buffer);
        munmap(shared_mem, 4096);
        prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
        return 1;
    }
    printf("PASS: Parent sees child's modifications (counter=%d, buffer='%s')\n",
           *shared_counter, shared_buffer);
    
    munmap(shared_mem, 4096);
    
    // Test 2: File-backed shared memory
    int fd = shm_open("/mitosis_test8", O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        printf("WARN: shm_open failed (may need /dev/shm): %s\n", strerror(errno));
    } else {
        ftruncate(fd, 4096);
        
        void *file_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (file_mem == MAP_FAILED) {
            printf("FAIL: File mmap failed: %s\n", strerror(errno));
            close(fd);
            shm_unlink("/mitosis_test8");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        
        // Write pattern
        memset(file_mem, 'X', 4096);
        ((char *)file_mem)[0] = 'S';
        ((char *)file_mem)[4095] = 'E';
        
        // Verify
        if (((char *)file_mem)[0] != 'S' || ((char *)file_mem)[4095] != 'E') {
            printf("FAIL: File-backed memory verification failed\n");
            munmap(file_mem, 4096);
            close(fd);
            shm_unlink("/mitosis_test8");
            prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
            return 1;
        }
        printf("PASS: File-backed shared memory works\n");
        
        munmap(file_mem, 4096);
        close(fd);
        shm_unlink("/mitosis_test8");
    }
    
    // Verify replication still enabled
    ret = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (ret <= 0) {
        printf("FAIL: Replication disabled after shared memory ops\n");
        return 1;
    }
    printf("PASS: Replication still enabled (0x%lx)\n", ret);
    
    // Clean up
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("\nTEST8: SUCCESS - Shared memory works with replication\n");
    return 0;
}
