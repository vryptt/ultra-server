#ifndef ULTRA_SERVER_H
#define ULTRA_SERVER_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdatomic.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <immintrin.h>

// Performance Configuration
#define PORT 8080
#define MAX_EVENTS 4096
#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 65536
#define BACKLOG 65536
#define SO_REUSEPORT_LB 512

// Thread Pool Configuration
#define MAX_WORKER_THREADS 32
#define MAX_IO_THREADS 8
#define THREAD_STACK_SIZE (1024 * 1024)

// Memory Pool Configuration
#define MEMORY_POOL_SIZE (64 * 1024 * 1024)
#define CONNECTION_POOL_SIZE 32768

// Cache Line Optimization
#define CACHE_LINE_SIZE 64
#define ALIGNED __attribute__((aligned(CACHE_LINE_SIZE)))

// Lock-free Ring Buffer
#define RING_BUFFER_SIZE 65536
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

typedef struct ALIGNED {
    atomic_ulong total_requests;
    atomic_ulong get_requests;
    atomic_ulong stats_requests;
    atomic_ulong active_connections;
    atomic_ulong bytes_sent;
    atomic_ulong bytes_received;
    atomic_ulong error_count;
    atomic_ulong peak_connections;
    atomic_ulong request_rate;
    time_t start_time;
    
    // Performance counters
    atomic_ulong cache_hits;
    atomic_ulong cache_misses;
    atomic_ulong thread_switches;
    atomic_ulong memory_allocations;
} server_stats_t;

typedef struct ALIGNED {
    int client_fd;
    struct sockaddr_in client_addr;
    uint64_t timestamp;
} connection_t;

typedef struct ALIGNED {
    atomic_uint head;
    atomic_uint tail;
    connection_t connections[RING_BUFFER_SIZE];
} ring_buffer_t;

typedef struct ALIGNED {
    int thread_id;
    int cpu_id;
    int epoll_fd;
    server_stats_t *stats;
    ring_buffer_t *ring_buffer;
    
    // Per-thread statistics
    atomic_ulong local_requests;
    atomic_ulong local_bytes_sent;
    
    // Memory pool
    void *memory_pool;
    size_t pool_offset;
    
    // Event notification
    int event_fd;
} worker_thread_t;

typedef struct ALIGNED {
    int server_fd;
    int epoll_fd;
    server_stats_t *stats;
    ring_buffer_t *ring_buffer;
    worker_thread_t *workers;
    int num_workers;
} io_thread_t;

// Pre-compiled responses for zero-copy
typedef struct {
    const char *data;
    size_t length;
} response_t;

// Function declarations
int create_optimized_server_socket(int port);
int set_socket_optimizations(int fd);
void setup_cpu_affinity(int thread_id, int total_threads);
void *io_thread_func(void *arg);
void *worker_thread_func(void *arg);
void handle_client_optimized(int client_fd, worker_thread_t *worker);
void send_response_optimized(int client_fd, const response_t *response, server_stats_t *stats);
void init_stats(server_stats_t *stats);
void init_ring_buffer(ring_buffer_t *buffer);
void *allocate_from_pool(worker_thread_t *worker, size_t size);
void prefetch_data(const void *addr);

// Lock-free ring buffer operations
static inline int ring_buffer_push(ring_buffer_t *buffer, const connection_t *conn) {
    uint32_t head = atomic_load_explicit(&buffer->head, memory_order_relaxed);
    uint32_t next_head = (head + 1) & RING_BUFFER_MASK;
    
    if (next_head == atomic_load_explicit(&buffer->tail, memory_order_acquire)) {
        return 0; // Buffer full
    }
    
    buffer->connections[head] = *conn;
    atomic_store_explicit(&buffer->head, next_head, memory_order_release);
    return 1;
}

static inline int ring_buffer_pop(ring_buffer_t *buffer, connection_t *conn) {
    uint32_t tail = atomic_load_explicit(&buffer->tail, memory_order_relaxed);
    
    if (tail == atomic_load_explicit(&buffer->head, memory_order_acquire)) {
        return 0; // Buffer empty
    }
    
    *conn = buffer->connections[tail];
    atomic_store_explicit(&buffer->tail, (tail + 1) & RING_BUFFER_MASK, memory_order_release);
    return 1;
}

// Fast string matching using SIMD
static inline int fast_path_match(const char *buffer, size_t len) {
    if (len < 14) return -1;
    
    // Check "GET " using 32-bit comparison
    if (*(uint32_t*)buffer != 0x20544547) return -1; // "GET " in little-endian
    
    // Fast path for "GET /"
    if (buffer[4] == '/' && buffer[5] == ' ') return 0;
    
    // Fast path for "GET /stats"
    if (len >= 20 && *(uint64_t*)(buffer + 4) == 0x73746174732f2f) {
        return 1;
    }
    
    return -1;
}

// Pre-compiled responses
extern const response_t ROOT_RESPONSE;
extern const response_t STATS_HEADER;
extern const response_t NOT_FOUND_RESPONSE;
extern const response_t BAD_REQUEST_RESPONSE;

#endif