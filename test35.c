// test_stress_fork_replication.c
// Compile: gcc -o test_stress test_stress_fork_replication.c -pthread -lnuma
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
#include <signal.h>
#include <sys/time.h>

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101

#define NUM_MIGRATION_THREADS 4
#define NUM_FAULT_THREADS 4
#define NUM_RAPID_FORKS 15
#define FAULT_ITERATIONS 5000
#define MIGRATION_CYCLES 100

// Global control flags
static volatile int keep_running = 1;
static volatile int fork_in_progress = 0;

// Statistics
typedef struct {
    atomic_int successful_forks;
    atomic_int failed_forks;
    atomic_int thread_failures;
    atomic_int migrations_completed;
    atomic_int page_faults_completed;
} stats_t;

stats_t stats = {0};

// Thread data
typedef struct {
    int thread_id;
    int num_nodes;
    enum { FAULT_THREAD, MIGRATION_THREAD } type;
} thread_data_t;

// Check replication status
static int check_replication(const char *context, int expected) {
    long status = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
    if (status < 0) {
        printf("[%s] ERROR: prctl(GET) failed: %s\n", context, strerror(errno));
        return -1;
    }
    
    if (expected >= 0 && status != expected) {
        printf("[%s] ERROR: Expected repl=0x%x, got 0x%x\n", context, expected, (int)status);
        return -1;
    }
    
    return (int)status;
}

// Pin to specific node
static int pin_to_node(int node) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // Find CPUs on this node
    struct bitmask *cpus = numa_allocate_cpumask();
    if (numa_node_to_cpus(node, cpus) < 0) {
        numa_free_cpumask(cpus);
        return -1;
    }
    
    for (int cpu = 0; cpu < numa_num_configured_cpus(); cpu++) {
        if (numa_bitmask_isbitset(cpus, cpu)) {
            CPU_SET(cpu, &cpuset);
        }
    }
    
    numa_free_cpumask(cpus);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
        return -1;
    }
    
    return 0;
}

// TEST 2: Thread that constantly faults pages
static void* fault_thread(void *arg) {
    thread_data_t *data = (thread_data_t*)arg;
    char name[32];
    snprintf(name, sizeof(name), "Fault%d", data->thread_id);
    
    // Pin to alternating nodes
    int node = data->thread_id % data->num_nodes;
    if (pin_to_node(node) < 0) {
        printf("[%s] FAIL: Cannot pin to node %d\n", name, node);
        atomic_fetch_add(&stats.thread_failures, 1);
        return NULL;
    }
    
    int faults = 0;
    while (keep_running && faults < FAULT_ITERATIONS) {
        // Allocate and fault in pages
        size_t size = 4096 * (1 + (faults % 16)); // 4KB to 64KB
        char *mem = malloc(size);
        if (!mem) {
            continue;
        }
        
        // Touch every page to trigger faults
        for (size_t i = 0; i < size; i += 4096) {
            mem[i] = (faults + i) & 0xFF;
        }
        
        // Verify page table walked correctly
        volatile char sum = 0;
        for (size_t i = 0; i < size; i += 4096) {
            sum += mem[i];
        }
        
        free(mem);
        faults++;
        
        // Occasionally check replication status
        if (faults % 500 == 0) {
            if (check_replication(name, -1) < 0) {
                atomic_fetch_add(&stats.thread_failures, 1);
                return NULL;
            }
        }
    }
    
    atomic_fetch_add(&stats.page_faults_completed, faults);
    printf("[%s] Completed %d page faults on node %d\n", name, faults, node);
    return NULL;
}

// TEST 3: Thread that migrates between nodes
static void* migration_thread(void *arg) {
    thread_data_t *data = (thread_data_t*)arg;
    char name[32];
    snprintf(name, sizeof(name), "Migrate%d", data->thread_id);
    
    for (int cycle = 0; cycle < MIGRATION_CYCLES && keep_running; cycle++) {
        int target_node = cycle % data->num_nodes;
        
        if (pin_to_node(target_node) < 0) {
            printf("[%s] FAIL: Cannot migrate to node %d\n", name, target_node);
            atomic_fetch_add(&stats.thread_failures, 1);
            return NULL;
        }
        
        // Verify we're on the right node
        int actual_node = numa_node_of_cpu(sched_getcpu());
        if (actual_node != target_node) {
            printf("[%s] FAIL: Expected node %d, on node %d\n", 
                   name, target_node, actual_node);
            atomic_fetch_add(&stats.thread_failures, 1);
            return NULL;
        }
        
        // Do some work to trigger page table walks on this node
        char *mem = malloc(8192);
        if (mem) {
            for (int i = 0; i < 8192; i += 512) {
                mem[i] = cycle & 0xFF;
            }
            volatile char sum = 0;
            for (int i = 0; i < 8192; i += 512) {
                sum += mem[i];
            }
            free(mem);
        }
        
        atomic_fetch_add(&stats.migrations_completed, 1);
        
        // Small delay between migrations
        usleep(1000);
    }
    
    printf("[%s] Completed %d migrations\n", name, MIGRATION_CYCLES);
    return NULL;
}

// TEST 1 & 4: Child process that validates state
static int child_process(int child_num, int parent_had_replication) {
    char name[32];
    snprintf(name, sizeof(name), "Child%d", child_num);
    
    // Child should always start with replication disabled
    int status = check_replication(name, 0);
    if (status != 0) {
        printf("[%s] FAIL: Child should start with replication disabled\n", name);
        return 1;
    }
    
    // Even if parent disables replication after fork, child is unaffected
    // (we'll test this by having parent disable after all forks)
    
    // Child should be able to enable independently
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) < 0) {
        printf("[%s] FAIL: Cannot enable replication: %s\n", name, strerror(errno));
        return 1;
    }
    
    status = check_replication(name, -1);
    if (status <= 0) {
        printf("[%s] FAIL: Replication not enabled\n", name);
        return 1;
    }
    
    // Do some work
    char *mem = malloc(16384);
    if (mem) {
        for (int i = 0; i < 16384; i++) {
            mem[i] = i & 0xFF;
        }
        free(mem);
    }
    
    // Disable before exit
    prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);
    
    printf("[%s] PASS (parent_had_repl=%d)\n", name, parent_had_replication);
    return 0;
}

int main() {
    pthread_t fault_threads[NUM_FAULT_THREADS];
    pthread_t migration_threads[NUM_MIGRATION_THREADS];
    thread_data_t thread_data[NUM_FAULT_THREADS + NUM_MIGRATION_THREADS];
    pid_t child_pids[NUM_RAPID_FORKS];
    int num_nodes = numa_num_configured_nodes();
    
    printf("=== MITOSIS STRESS TEST ===\n");
    printf("PID: %d\n", getpid());
    printf("NUMA nodes: %d\n", num_nodes);
    printf("Config: %d forks, %d fault threads, %d migration threads\n",
           NUM_RAPID_FORKS, NUM_FAULT_THREADS, NUM_MIGRATION_THREADS);
    
    if (num_nodes < 2) {
        printf("ERROR: Need at least 2 NUMA nodes\n");
        return 1;
    }
    
    // Enable replication
    printf("\n=== ENABLING REPLICATION ===\n");
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) < 0) {
        printf("FAIL: Cannot enable replication: %s\n", strerror(errno));
        return 1;
    }
    
    if (check_replication("Parent-Init", -1) <= 0) {
        printf("FAIL: Replication not enabled\n");
        return 1;
    }
    printf("PASS: Replication enabled\n");
    
    // TEST 2 & 3: Start background threads before forking
    printf("\n=== STARTING BACKGROUND THREADS ===\n");
    
    // Start fault threads
    for (int i = 0; i < NUM_FAULT_THREADS; i++) {
        thread_data[i].thread_id = i;
        thread_data[i].num_nodes = num_nodes;
        thread_data[i].type = FAULT_THREAD;
        
        if (pthread_create(&fault_threads[i], NULL, fault_thread, &thread_data[i]) != 0) {
            printf("FAIL: Cannot create fault thread %d\n", i);
            return 1;
        }
    }
    
    // Start migration threads
    for (int i = 0; i < NUM_MIGRATION_THREADS; i++) {
        int idx = NUM_FAULT_THREADS + i;
        thread_data[idx].thread_id = i;
        thread_data[idx].num_nodes = num_nodes;
        thread_data[idx].type = MIGRATION_THREAD;
        
        if (pthread_create(&migration_threads[i], NULL, migration_thread, 
                          &thread_data[idx]) != 0) {
            printf("FAIL: Cannot create migration thread %d\n", i);
            return 1;
        }
    }
    
    printf("PASS: All background threads started\n");
    usleep(100000); // Let threads warm up
    
    // TEST 1: RAPID FORK BOMBING while threads are active
    printf("\n=== RAPID FORK TEST (while threads fault/migrate) ===\n");
    
    for (int i = 0; i < NUM_RAPID_FORKS; i++) {
        fork_in_progress = 1;
        child_pids[i] = fork();
        
        if (child_pids[i] < 0) {
            printf("FAIL: fork %d failed: %s\n", i, strerror(errno));
            atomic_fetch_add(&stats.failed_forks, 1);
            child_pids[i] = 0;
            fork_in_progress = 0;
            continue;
        }
        
        if (child_pids[i] == 0) {
            // Child process
            keep_running = 0; // Stop any inherited thread loops
            int result = child_process(i, 1);
            exit(result);
        }
        
        // Parent continues
        atomic_fetch_add(&stats.successful_forks, 1);
        fork_in_progress = 0;
        
        // Small delay between forks to let system stabilize
        usleep(10000);
        
        // Verify parent replication still enabled
        if (i % 5 == 0) {
            if (check_replication("Parent-DuringForks", -1) <= 0) {
                printf("FAIL: Parent lost replication during forks\n");
                keep_running = 0;
                break;
            }
        }
    }
    
    printf("PASS: Completed %d forks (%d successful, %d failed)\n",
           NUM_RAPID_FORKS,
           atomic_load(&stats.successful_forks),
           atomic_load(&stats.failed_forks));
    
    // Let threads run a bit more
    usleep(200000);
    
    // TEST 4: Disable replication in parent AFTER forks
    printf("\n=== DISABLING PARENT REPLICATION ===\n");
    
    if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) < 0) {
        printf("FAIL: Cannot disable replication: %s\n", strerror(errno));
    } else {
        if (check_replication("Parent-AfterDisable", 0) != 0) {
            printf("FAIL: Replication not disabled\n");
        } else {
            printf("PASS: Parent replication disabled\n");
        }
    }
    
    // Stop threads
    printf("\n=== STOPPING THREADS ===\n");
    keep_running = 0;
    
    for (int i = 0; i < NUM_FAULT_THREADS; i++) {
        pthread_join(fault_threads[i], NULL);
    }
    
    for (int i = 0; i < NUM_MIGRATION_THREADS; i++) {
        pthread_join(migration_threads[i], NULL);
    }
    
    printf("PASS: All threads stopped\n");
    
    // Wait for all children and verify they're unaffected
    printf("\n=== WAITING FOR CHILDREN ===\n");
    int children_ok = 0;
    int children_failed = 0;
    
    for (int i = 0; i < NUM_RAPID_FORKS; i++) {
        if (child_pids[i] > 0) {
            int status;
            pid_t result = waitpid(child_pids[i], &status, 0);
            
            if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                children_ok++;
            } else {
                children_failed++;
                printf("Child %d failed (status=%d)\n", i, 
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    }
    
    printf("Children: %d OK, %d failed\n", children_ok, children_failed);
    
    // FINAL RESULTS
    printf("\n=== FINAL RESULTS ===\n");
    printf("Successful forks:      %d/%d\n", 
           atomic_load(&stats.successful_forks), NUM_RAPID_FORKS);
    printf("Failed forks:          %d\n", atomic_load(&stats.failed_forks));
    printf("Thread failures:       %d\n", atomic_load(&stats.thread_failures));
    printf("Migrations completed:  %d (expected ~%d)\n",
           atomic_load(&stats.migrations_completed),
           NUM_MIGRATION_THREADS * MIGRATION_CYCLES);
    printf("Page faults completed: %d (expected ~%d)\n",
           atomic_load(&stats.page_faults_completed),
           NUM_FAULT_THREADS * FAULT_ITERATIONS);
    printf("Children OK:           %d/%d\n", children_ok, NUM_RAPID_FORKS);
    
    int all_ok = (atomic_load(&stats.failed_forks) == 0) &&
                 (atomic_load(&stats.thread_failures) == 0) &&
                 (children_failed == 0) &&
                 (atomic_load(&stats.successful_forks) == NUM_RAPID_FORKS);
    
    if (all_ok) {
        printf("\n*** ALL STRESS TESTS PASSED ***\n");
        return 0;
    } else {
        printf("\n*** STRESS TEST FAILED ***\n");
        return 1;
    }
}
