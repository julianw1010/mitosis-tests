// test24.c - Userfaultfd interaction with page table replication
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>  // ADD THIS for O_CLOEXEC and O_NONBLOCK

#define PR_SET_PGTABLE_REPL 100
#define PR_GET_PGTABLE_REPL 101
#define PAGE_SIZE 4096
#define NUM_PAGES 8

static int uffd;
static void *fault_region;
static pthread_t fault_thread;
static volatile int handler_ready = 0;
static volatile int test_complete = 0;

// Fault handler thread
static void *fault_handler(void *arg) {
    struct uffd_msg msg;
    struct uffdio_copy copy;
    void *page;
    int handled = 0;
    
    // Allocate page for copying
    page = malloc(PAGE_SIZE);
    if (!page) {
        printf("Handler: Failed to allocate page\n");
        return NULL;
    }
    
    handler_ready = 1;
    printf("Handler: Ready and waiting for faults...\n");
    
    while (!test_complete) {
        struct pollfd pollfd = {
            .fd = uffd,
            .events = POLLIN
        };
        
        int ret = poll(&pollfd, 1, 100);  // 100ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        
        if (ret == 0) continue;  // Timeout
        
        // Read fault event
        ret = read(uffd, &msg, sizeof(msg));
        if (ret < 0) {
            if (errno == EAGAIN) continue;
            perror("read uffd");
            break;
        }
        
        if (ret != sizeof(msg)) {
            printf("Handler: Partial read\n");
            continue;
        }
        
        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            printf("Handler: Unexpected event %d\n", msg.event);
            continue;
        }
        
        // Prepare page with pattern based on fault address
        unsigned long addr = msg.arg.pagefault.address;
        int page_num = (addr - (unsigned long)fault_region) / PAGE_SIZE;
        memset(page, 0xAA + page_num, PAGE_SIZE);
        *(uint32_t *)page = 0xDEAD0000 | page_num;
        
        printf("Handler: Fault at %lx (page %d), providing page\n", addr, page_num);
        
        // Resolve the fault
        copy.src = (unsigned long)page;
        copy.dst = addr & ~(PAGE_SIZE - 1);
        copy.len = PAGE_SIZE;
        copy.mode = 0;
        copy.copy = 0;
        
        if (ioctl(uffd, UFFDIO_COPY, &copy) < 0) {
            perror("UFFDIO_COPY");
            continue;
        }
        
        handled++;
    }
    
    printf("Handler: Handled %d faults, exiting\n", handled);
    free(page);
    return NULL;
}

int main(void) {
    struct uffdio_api api;
    struct uffdio_register reg;
    int i;
    int pass = 1;
    
    printf("Test 24: Userfaultfd with Replicated Page Tables\n");
    printf("================================================\n");
    
    // Create userfaultfd
    uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (uffd < 0) {
        perror("userfaultfd");
        printf("NOTE: userfaultfd not available (needs root or CAP_SYS_PTRACE)\n");
        return 0;  // Skip test if not available
    }
    
    // Enable API
    api.api = UFFD_API;
    api.features = 0;
    if (ioctl(uffd, UFFDIO_API, &api) < 0) {
        perror("UFFDIO_API");
        close(uffd);
        return 1;
    }
    
    // Allocate monitored region
    fault_region = mmap(NULL, PAGE_SIZE * NUM_PAGES,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fault_region == MAP_FAILED) {
        perror("mmap");
        close(uffd);
        return 1;
    }
    
    printf("Allocated fault region at %p\n", fault_region);
    
    // Register with userfaultfd
    reg.range.start = (unsigned long)fault_region;
    reg.range.len = PAGE_SIZE * NUM_PAGES;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0) {
        perror("UFFDIO_REGISTER");
        munmap(fault_region, PAGE_SIZE * NUM_PAGES);
        close(uffd);
        return 1;
    }
    
    // Start fault handler thread
    if (pthread_create(&fault_thread, NULL, fault_handler, NULL) != 0) {
        perror("pthread_create");
        munmap(fault_region, PAGE_SIZE * NUM_PAGES);
        close(uffd);
        return 1;
    }
    
    // Wait for handler to be ready
    while (!handler_ready) {
        usleep(1000);
    }
    
    printf("\n--- Testing with replication disabled ---\n");
    
    // Access pages (will trigger faults)
    uint32_t *ptr = (uint32_t *)fault_region;
    for (i = 0; i < NUM_PAGES; i++) {
        uint32_t val = ptr[i * PAGE_SIZE / sizeof(uint32_t)];
        uint32_t expected = 0xDEAD0000 | i;
        if (val != expected) {
            printf("ERROR: Page %d wrong value: got 0x%x, expected 0x%x\n",
                   i, val, expected);
            pass = 0;
        } else {
            printf("Page %d: Correct value 0x%x after fault\n", i, val);
        }
    }
    
    // Now enable replication
    printf("\n--- Enabling replication ---\n");
    if (prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0) != 0) {
        perror("prctl(PR_SET_PGTABLE_REPL)");
        pass = 0;
    } else {
        unsigned long mask = prctl(PR_GET_PGTABLE_REPL, 0, 0, 0, 0);
        printf("Replication enabled with mask: 0x%lx\n", mask);
        
        // Verify existing pages still accessible
        printf("\n--- Verifying existing pages with replication ---\n");
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t val = ptr[i * PAGE_SIZE / sizeof(uint32_t)];
            uint32_t expected = 0xDEAD0000 | i;
            if (val != expected) {
                printf("ERROR: Page %d corrupted after enable: got 0x%x\n", i, val);
                pass = 0;
            }
        }
        
        // Unmap half the pages to trigger new faults
        printf("\n--- Unmapping half the pages ---\n");
        if (madvise(fault_region, PAGE_SIZE * (NUM_PAGES/2), MADV_DONTNEED) != 0) {
            perror("madvise(MADV_DONTNEED)");
            pass = 0;
        }
        
        // Re-access unmapped pages (new faults with replication active)
        printf("\n--- Re-faulting pages with replication active ---\n");
        for (i = 0; i < NUM_PAGES/2; i++) {
            uint32_t val = ptr[i * PAGE_SIZE / sizeof(uint32_t)];
            uint32_t expected = 0xDEAD0000 | i;
            if (val != expected) {
                printf("ERROR: Re-faulted page %d wrong: got 0x%x, expected 0x%x\n",
                       i, val, expected);
                pass = 0;
            } else {
                printf("Page %d: Correctly re-faulted with value 0x%x\n", i, val);
            }
        }
        
        // Write to pages
        printf("\n--- Writing to userfault pages ---\n");
        for (i = 0; i < NUM_PAGES; i++) {
            ptr[i * PAGE_SIZE / sizeof(uint32_t)] = 0xBEEF0000 | i;
        }
        
        // Verify writes
        for (i = 0; i < NUM_PAGES; i++) {
            uint32_t val = ptr[i * PAGE_SIZE / sizeof(uint32_t)];
            uint32_t expected = 0xBEEF0000 | i;
            if (val != expected) {
                printf("ERROR: Write failed on page %d: got 0x%x\n", i, val);
                pass = 0;
            }
        }
        
        // Disable replication
        if (prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0) != 0) {
            perror("prctl(PR_SET_PGTABLE_REPL disable)");
            pass = 0;
        }
    }
    
    // Signal handler to exit
    test_complete = 1;
    pthread_join(fault_thread, NULL);
    
    // Cleanup
    munmap(fault_region, PAGE_SIZE * NUM_PAGES);
    close(uffd);
    
    if (pass) {
        printf("\n*** TEST 24 PASSED ***\n");
        printf("Userfaultfd works correctly with replication\n");
    } else {
        printf("\n*** TEST 24 FAILED ***\n");
        printf("Issues with userfaultfd under replication\n");
    }
    
    return pass ? 0 : 1;
}
