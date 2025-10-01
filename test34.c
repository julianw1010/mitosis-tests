// Compile: gcc -o test34 test_thread_fork_replication.c -pthread -lnuma
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <numa.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#include <stdatomic.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

#define NUM_THREADS 4
#define ITERATIONS 1000

// Test results tracking
typedef struct {
    atomic_int parent_threads_ok;
    atomic_int failures;
} test_results_t;

test_results_t results = {0};

// Per-thread data
typedef struct {
    int thread_id;
    int target_node;
    int phase; // 0=before_fork, 1=after_fork
} thread_data_t;

// Memory access pattern to trigger page table walks
static void trigger_page_walks(int iterations) {
    volatile long sum = 0;
    for (int i = 0; i < iterations; i++) {
        char *mem = malloc(4096);
        if (mem) {
            mem[0] = i & 0xFF;
            mem[4095] = (i >> 8) & 0xFF;
            sum += mem[0] + mem[4095];
            free(mem);
        }
    }
}

// Check replication status
static int check_replication(const char *context) {
    long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status < 0) {
        printf("[%s] FAIL: prctl(GET) failed: %s\n", context, strerror(errno));
        return -1;
    }
    return (int)status;
}

// Pin thread to specific node
static int pin_to_node(int node) {
    struct bitmask *mask = numa_allocate_nodemask();
    numa_bitmask_setbit(mask, node);
    
    if (numa_run_on_node_mask(mask) < 0) {
        numa_free_nodemask(mask);
        return -1;
    }
    
    numa_free_nodemask(mask);
    return 0;
}

// Thread worker function
static void* thread_worker(void *arg) {
    thread_data_t *data = (thread_data_t*)arg;
    int node = data->target_node;
    
    printf("[T%d] DEBUG: Thread started, data=%p, tid=%d, node=%d, phase=%d\n",
           data->thread_id, data, data->thread_id, node, data->phase);
    fflush(stdout);
    
    // Pin to target node
    if (pin_to_node(node) < 0) {
        printf("[T%d Phase%d] FAIL: Cannot pin to node %d\n", 
               data->thread_id, data->phase, node);
        atomic_fetch_add(&results.failures, 1);
        return NULL;
    }
    
    // Verify we're on the right node
    int actual_node = numa_node_of_cpu(sched_getcpu());
    if (actual_node != node) {
        printf("[T%d Phase%d] FAIL: Expected node %d, got %d\n",
               data->thread_id, data->phase, node, actual_node);
        atomic_fetch_add(&results.failures, 1);
        return NULL;
    }
    
    // Check replication status
    char context[64];
    snprintf(context, sizeof(context), "T%d Phase%d Node%d", 
             data->thread_id, data->phase, node);
    int repl_status = check_replication(context);
    
    if (repl_status < 0) {
        atomic_fetch_add(&results.failures, 1);
        return NULL;
    }
    
    if (repl_status == 0) {
        printf("[%s] FAIL: Replication unexpectedly disabled\n", context);
        atomic_fetch_add(&results.failures, 1);
        return NULL;
    }
    
    // Trigger page table operations
    trigger_page_walks(ITERATIONS);
    
    printf("[%s] PASS: Thread completed successfully (repl_mask=0x%x)\n", 
           context, repl_status);
    fflush(stdout);
    
    // DEBUG: Check phase value before increment decision
    int local_phase = data->phase;
    printf("[T%d] DEBUG: About to check phase, local_phase=%d, data->phase=%d\n",
           data->thread_id, local_phase, data->phase);
    fflush(stdout);
    
    if (data->phase == 1) {
        int before = atomic_load(&results.parent_threads_ok);
        printf("[T%d] DEBUG: Phase==1, incrementing counter (before=%d)\n",
               data->thread_id, before);
        fflush(stdout);
        
        int after = atomic_fetch_add(&results.parent_threads_ok, 1) + 1;
        
        printf("[T%d] DEBUG: Counter incremented (after=%d)\n",
               data->thread_id, after);
        fflush(stdout);
    } else {
        printf("[T%d] DEBUG: Phase!=%d, NOT incrementing counter\n",
               data->thread_id, data->phase);
        fflush(stdout);
    }
    
    return NULL;
}

// Run child process test
static int test_child_process() {
    printf("\n=== CHILD PROCESS TEST ===\n");
    printf("[Child PID=%d] Started\n", getpid());
    fflush(stdout);
    
    // 1. Verify replication is DISABLED in child
    int status = check_replication("Child-Initial");
    if (status < 0) return -1;
    
    if (status != 0) {
        printf("[Child] FAIL: Replication should be disabled, got 0x%x\n", status);
        return -1;
    }
    printf("[Child] PASS: Replication correctly disabled after fork\n");
    
    // 2. Child should be able to independently enable replication
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) < 0) {
        printf("[Child] FAIL: Cannot enable replication: %s\n", strerror(errno));
        return -1;
    }
    
    status = check_replication("Child-AfterEnable");
    if (status <= 0) {
        printf("[Child] FAIL: Enable replication failed\n");
        return -1;
    }
    printf("[Child] PASS: Child can independently enable replication (mask=0x%x)\n", status);
    
    // 3. Test child thread on different node
    pthread_t thread;
    thread_data_t data = {.thread_id = 99, .target_node = 1, .phase = 2};
    
    printf("[Child] DEBUG: Creating child thread, phase=%d\n", data.phase);
    fflush(stdout);
    
    if (pthread_create(&thread, NULL, thread_worker, &data) != 0) {
        printf("[Child] FAIL: Cannot create thread\n");
        return -1;
    }
    
    pthread_join(thread, NULL);
    
    printf("[Child] PASS: All child tests completed\n");
    fflush(stdout);
    
    return 0;
}

int main() {
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    pid_t child_pid;
    int num_nodes = numa_num_configured_nodes();
    
    printf("=== MITOSIS THREAD-FORK REPLICATION TEST ===\n");
    printf("PID: %d\n", getpid());
    printf("NUMA nodes available: %d\n", num_nodes);
    printf("DEBUG: thread_data array at %p\n", thread_data);
    fflush(stdout);
    
    if (num_nodes < 2) {
        printf("ERROR: Need at least 2 NUMA nodes\n");
        return 1;
    }
    
    // PHASE 1: Enable replication in parent
    printf("\n=== PHASE 1: ENABLE REPLICATION ===\n");
    
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) < 0) {
        printf("FAIL: Cannot enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    int status = check_replication("Parent-Initial");
    if (status <= 0) {
        printf("FAIL: Replication not enabled\n");
        return 1;
    }
    printf("PASS: Replication enabled (mask=0x%x)\n", status);
    
    // PHASE 2: Spawn threads on different nodes BEFORE fork
    printf("\n=== PHASE 2: CREATE THREADS (PRE-FORK) ===\n");
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].target_node = i % num_nodes;
        thread_data[i].phase = 0;
        
        printf("DEBUG: Creating thread %d: data[%d]=%p, phase=%d\n",
               i, i, &thread_data[i], thread_data[i].phase);
        fflush(stdout);
        
        if (pthread_create(&threads[i], NULL, thread_worker, &thread_data[i]) != 0) {
            printf("FAIL: Cannot create thread %d\n", i);
            return 1;
        }
    }
    
    // Wait for pre-fork threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    if (atomic_load(&results.failures) > 0) {
        printf("FAIL: Pre-fork threads encountered errors\n");
        return 1;
    }
    printf("PASS: All pre-fork threads completed\n");
    
    // PHASE 3: Fork while replication is active
    printf("\n=== PHASE 3: FORK TEST ===\n");
    fflush(stdout);
    
    child_pid = fork();
    
    if (child_pid < 0) {
        printf("FAIL: fork() failed: %s\n", strerror(errno));
        return 1;
    }
    
    if (child_pid == 0) {
        // CHILD PROCESS
        int child_result = test_child_process();
        exit(child_result == 0 ? 0 : 1);
    }
    
    // PARENT PROCESS
    printf("\n=== PHASE 4: PARENT POST-FORK TEST ===\n");
    printf("[Parent PID=%d] Continuing after fork\n", getpid());
    printf("DEBUG: Counter before post-fork threads: %d\n", 
           atomic_load(&results.parent_threads_ok));
    fflush(stdout);
    
    // Verify parent replication still enabled
    status = check_replication("Parent-AfterFork");
    if (status <= 0) {
        printf("[Parent] FAIL: Replication disabled after fork\n");
        return 1;
    }
    printf("[Parent] PASS: Replication still enabled (mask=0x%x)\n", status);
    
    // CRITICAL: Zero out or reinitialize the array to avoid stale data
    memset(thread_data, 0, sizeof(thread_data));
    
    // Spawn new threads AFTER fork
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i + 100;
        thread_data[i].target_node = i % num_nodes;
        thread_data[i].phase = 1;
        
        printf("DEBUG: Creating post-fork thread %d: data[%d]=%p, phase=%d\n",
               i, i, &thread_data[i], thread_data[i].phase);
        fflush(stdout);
        
        if (pthread_create(&threads[i], NULL, thread_worker, &thread_data[i]) != 0) {
            printf("[Parent] FAIL: Cannot create post-fork thread %d\n", i);
            return 1;
        }
    }
    
    // Wait for post-fork threads
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("DEBUG: Joining post-fork thread %d\n", i);
        fflush(stdout);
        pthread_join(threads[i], NULL);
    }
    
    printf("DEBUG: Counter after all post-fork threads: %d\n",
           atomic_load(&results.parent_threads_ok));
    fflush(stdout);
    
    // Wait for child
    int child_status;
    waitpid(child_pid, &child_status, 0);
    
    // FINAL RESULTS
    printf("\n=== FINAL RESULTS ===\n");
    printf("Failures: %d\n", atomic_load(&results.failures));
    printf("Parent post-fork threads OK: %d/%d\n", 
           atomic_load(&results.parent_threads_ok), NUM_THREADS);
    
    int child_ok = WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
    printf("Child test OK: %s (exit status: %d)\n", 
           child_ok ? "YES" : "NO",
           WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1);
    
    int total_ok = (atomic_load(&results.failures) == 0) &&
                   (atomic_load(&results.parent_threads_ok) == NUM_THREADS) &&
                   child_ok;
    
    if (total_ok) {
        printf("\n*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        printf("\n*** TEST FAILED ***\n");
        return 1;
    }
}
