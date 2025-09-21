#include "ultra_server.h"

static server_stats_t global_stats;
static ring_buffer_t connection_buffer;
static worker_thread_t workers[MAX_WORKER_THREADS];
static int num_cpu_cores;
static int num_worker_threads;
static int num_io_threads;

static void setup_process_optimizations(void) {
    // Set process priority
    if (setpriority(PRIO_PROCESS, 0, -20) == -1) {
        perror("Warning: Could not set high priority");
    }
    
    // Lock memory to prevent swapping
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        perror("Warning: Could not lock memory");
    }
    
    // Set CPU governor to performance (requires root)
    system("echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null 2>&1");
}

static void detect_system_capabilities(void) {
    num_cpu_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("Detected %d CPU cores\n", num_cpu_cores);
    
    // Optimize thread counts based on system
    num_worker_threads = num_cpu_cores * 2;
    if (num_worker_threads > MAX_WORKER_THREADS) {
        num_worker_threads = MAX_WORKER_THREADS;
    }
    
    num_io_threads = (num_cpu_cores >= 8) ? 4 : 2;
    if (num_io_threads > MAX_IO_THREADS) {
        num_io_threads = MAX_IO_THREADS;
    }
    
    printf("Using %d worker threads, %d I/O threads\n", num_worker_threads, num_io_threads);
}

static void setup_signal_handling(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_DFL);
}

//int main(int argc, char *argv[]) {
int main(void) {
    printf("Ultra-Fast Multi-Core HTTP Server v2.0\n");
    printf("=======================================\n");
    
    // Check for root privileges for optimizations
    if (geteuid() == 0) {
        printf("Running with root privileges - enabling all optimizations\n");
    } else {
        printf("Warning: Not running as root - some optimizations disabled\n");
    }
    
    setup_signal_handling();
    setup_process_optimizations();
    detect_system_capabilities();
    
    // Initialize global structures
    init_stats(&global_stats);
    init_ring_buffer(&connection_buffer);
    
    // Create optimized server socket
    int server_fd = create_optimized_server_socket(PORT);
    if (server_fd == -1) {
        fprintf(stderr, "Failed to create server socket\n");
        exit(EXIT_FAILURE);
    }
    
    printf("Server socket created and optimized\n");
    
    // Create worker threads with CPU affinity
    pthread_t worker_threads[MAX_WORKER_THREADS];
    pthread_attr_t attr;
    
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    for (int i = 0; i < num_worker_threads; i++) {
        workers[i].thread_id = i;
        workers[i].cpu_id = i % num_cpu_cores;
        workers[i].stats = &global_stats;
        workers[i].ring_buffer = &connection_buffer;
        workers[i].event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        
        // Allocate per-thread memory pool
        workers[i].memory_pool = mmap(NULL, MEMORY_POOL_SIZE, 
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (workers[i].memory_pool == MAP_FAILED) {
            perror("mmap memory pool");
            exit(EXIT_FAILURE);
        }
        workers[i].pool_offset = 0;
        
        if (pthread_create(&worker_threads[i], &attr, worker_thread_func, &workers[i]) != 0) {
            perror("pthread_create worker");
            exit(EXIT_FAILURE);
        }
    }
    
    printf("Created %d worker threads\n", num_worker_threads);
    
    // Create I/O threads
    pthread_t io_threads[MAX_IO_THREADS];
    io_thread_t io_workers[MAX_IO_THREADS];
    
    for (int i = 0; i < num_io_threads; i++) {
        io_workers[i].server_fd = server_fd;
        io_workers[i].stats = &global_stats;
        io_workers[i].ring_buffer = &connection_buffer;
        io_workers[i].workers = workers;
        io_workers[i].num_workers = num_worker_threads;
        
        // Create epoll instance for each I/O thread
        io_workers[i].epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (io_workers[i].epoll_fd == -1) {
            perror("epoll_create1");
            exit(EXIT_FAILURE);
        }
        
        if (pthread_create(&io_threads[i], &attr, io_thread_func, &io_workers[i]) != 0) {
            perror("pthread_create io");
            exit(EXIT_FAILURE);
        }
    }
    
    printf("Created %d I/O threads\n", num_io_threads);
    
    pthread_attr_destroy(&attr);
    
    printf("\nServer running on port %d\n", PORT);
    printf("Endpoints:\n");
    printf("  GET /      - Ultra-fast root endpoint\n");
    printf("  GET /stats - Comprehensive server statistics\n");
    printf("\nOptimizations enabled:\n");
    printf("  - Multi-core CPU affinity\n");
    printf("  - Lock-free ring buffers\n");
    printf("  - Memory pools per thread\n");
    printf("  - SIMD string matching\n");
    printf("  - Zero-copy responses\n");
    printf("  - SO_REUSEPORT load balancing\n");
    printf("  - TCP_NODELAY and optimizations\n");
    
    // Statistics reporting thread
    time_t last_report = time(NULL);
    uint64_t last_requests = 0;
    
    while (1) {
        sleep(5);
        
        time_t now = time(NULL);
        uint64_t current_requests = atomic_load(&global_stats.total_requests);
        
        if (now - last_report >= 5) {
            double rps = (current_requests - last_requests) / 5.0;
            atomic_store(&global_stats.request_rate, (uint64_t)rps);
            
            printf("\n=== Performance Stats ===\n");
            printf("RPS: %.0f | Total: %lu | Active: %lu | Errors: %lu\n",
                   rps, current_requests, 
                   atomic_load(&global_stats.active_connections),
                   atomic_load(&global_stats.error_count));
            
            last_report = now;
            last_requests = current_requests;
        }
    }
    
    // Cleanup (never reached in normal operation)
    for (int i = 0; i < num_worker_threads; i++) {
        pthread_cancel(worker_threads[i]);
        pthread_join(worker_threads[i], NULL);
        munmap(workers[i].memory_pool, MEMORY_POOL_SIZE);
        close(workers[i].event_fd);
    }
    
    for (int i = 0; i < num_io_threads; i++) {
        pthread_cancel(io_threads[i]);
        pthread_join(io_threads[i], NULL);
        close(io_workers[i].epoll_fd);
    }
    
    close(server_fd);
    return 0;
}