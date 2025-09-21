#include "ultra_server.h"

// Pre-compiled responses for zero-copy performance
const response_t ROOT_RESPONSE = {
    .data = "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/plain\r\n"
           "Content-Length: 2\r\n"
           "Connection: keep-alive\r\n"
           "Cache-Control: max-age=3600\r\n"
           "\r\n"
           "ok",
    .length = 89
};

const response_t STATS_HEADER = {
    .data = "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Connection: keep-alive\r\n"
           "Cache-Control: no-cache\r\n"
           "\r\n",
    .length = 88
};

const response_t NOT_FOUND_RESPONSE = {
    .data = "HTTP/1.1 404 Not Found\r\n"
           "Content-Length: 0\r\n"
           "Connection: keep-alive\r\n"
           "\r\n",
    .length = 57
};

const response_t BAD_REQUEST_RESPONSE = {
    .data = "HTTP/1.1 400 Bad Request\r\n"
           "Content-Length: 0\r\n"
           "Connection: keep-alive\r\n"
           "\r\n",
    .length = 59
};

void init_stats(server_stats_t *stats) {
    memset(stats, 0, sizeof(server_stats_t));
    stats->start_time = time(NULL);
}

void init_ring_buffer(ring_buffer_t *buffer) {
    atomic_store(&buffer->head, 0);
    atomic_store(&buffer->tail, 0);
    memset(buffer->connections, 0, sizeof(buffer->connections));
}

int create_optimized_server_socket(int port) {
    int server_fd;
    struct sockaddr_in address;
    
    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd == -1) {
        perror("socket");
        return -1;
    }
    
    if (set_socket_optimizations(server_fd) == -1) {
        close(server_fd);
        return -1;
    }
    
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }
    
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }
    
    return server_fd;
}

int set_socket_optimizations(int fd) {
    int opt = 1;
    
    // Enable address and port reuse
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("SO_REUSEADDR");
        return -1;
    }
    
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("SO_REUSEPORT");
        return -1;
    }
    
    // Optimize TCP performance
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        perror("TCP_NODELAY");
        return -1;
    }
    
    // Set large buffers
    int bufsize = 1024 * 1024; // 1MB
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    
    // Enable TCP fast open
    opt = 5;
    setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &opt, sizeof(opt));
    
    // Set to non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    
    return 0;
}

void setup_cpu_affinity(int thread_id, int total_threads) {
    cpu_set_t cpuset;
    int cpu_id = thread_id % sysconf(_SC_NPROCESSORS_ONLN);
    
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        perror("pthread_setaffinity_np");
    }
    
    // Set thread name for debugging
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "worker-%d", thread_id);
    pthread_setname_np(pthread_self(), thread_name);
}

void *allocate_from_pool(worker_thread_t *worker, size_t size) {
    // Align to cache line
    size = (size + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    
    if (worker->pool_offset + size > MEMORY_POOL_SIZE) {
        // Reset pool (simple strategy)
        worker->pool_offset = 0;
        atomic_fetch_add(&worker->stats->memory_allocations, 1);
    }
    
    void *ptr = (char*)worker->memory_pool + worker->pool_offset;
    worker->pool_offset += size;
    return ptr;
}

void prefetch_data(const void *addr) {
    prefetch_data_inline(addr);
}

void *io_thread_func(void *arg) {
    io_thread_t *io_worker = (io_thread_t*)arg;
    struct epoll_event event, events[MAX_EVENTS];
    int worker_idx = 0;
    
    // Set CPU affinity for I/O thread
    setup_cpu_affinity(io_worker - (io_thread_t*)arg + MAX_WORKER_THREADS, MAX_WORKER_THREADS + MAX_IO_THREADS);
    
    // Add server socket to epoll
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = io_worker->server_fd;
    if (epoll_ctl(io_worker->epoll_fd, EPOLL_CTL_ADD, io_worker->server_fd, &event) == -1) {
        perror("epoll_ctl server");
        return NULL;
    }
    
    printf("I/O thread started\n");
    
    while (1) {
        int nfds = epoll_wait(io_worker->epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno != EINTR) {
                perror("epoll_wait");
            }
            continue;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == io_worker->server_fd) {
                // Accept connections in batch
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept4(io_worker->server_fd, 
                                          (struct sockaddr*)&client_addr, 
                                          &client_len, 
                                          SOCK_NONBLOCK | SOCK_CLOEXEC);
                    
                    if (client_fd == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            atomic_fetch_add(&io_worker->stats->error_count, 1);
                        }
                        break;
                    }
                    
                    // Set client socket optimizations
                    set_socket_optimizations(client_fd);
                    
                    // Push to ring buffer for worker threads
                    connection_t conn = {
                        .client_fd = client_fd,
                        .client_addr = client_addr,
                        .timestamp = get_cpu_cycles()
                    };
                    
                    if (!ring_buffer_push(io_worker->ring_buffer, &conn)) {
                        // Ring buffer full, drop connection
                        close(client_fd);
                        atomic_fetch_add(&io_worker->stats->error_count, 1);
                        continue;
                    }
                    
                    // Notify worker thread using round-robin
                    uint64_t notify = 1;
                    write(io_worker->workers[worker_idx].event_fd, &notify, sizeof(notify));
                    worker_idx = (worker_idx + 1) % io_worker->num_workers;
                    
                    atomic_fetch_add(&io_worker->stats->active_connections, 1);
                    
                    // Update peak connections
                    uint64_t current = atomic_load(&io_worker->stats->active_connections);
                    uint64_t peak = atomic_load(&io_worker->stats->peak_connections);
                    if (current > peak) {
                        atomic_compare_exchange_weak(&io_worker->stats->peak_connections, &peak, current);
                    }
                }
            }
        }
    }
    
    return NULL;
}

void *worker_thread_func(void *arg) {
    worker_thread_t *worker = (worker_thread_t*)arg;
    struct epoll_event event, events[MAX_EVENTS];
    connection_t conn;
    
    // Set CPU affinity
    setup_cpu_affinity(worker->thread_id, MAX_WORKER_THREADS);
    
    // Create epoll for this worker
    worker->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker->epoll_fd == -1) {
        perror("epoll_create1 worker");
        return NULL;
    }
    
    // Add event fd to epoll
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = worker->event_fd;
    if (epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, worker->event_fd, &event) == -1) {
        perror("epoll_ctl event_fd");
        return NULL;
    }
    
    printf("Worker thread %d started on CPU %d\n", worker->thread_id, worker->cpu_id);
    
    while (1) {
        int nfds = epoll_wait(worker->epoll_fd, events, MAX_EVENTS, 100);
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == worker->event_fd) {
                // Read notification
                uint64_t notify;
                read(worker->event_fd, &notify, sizeof(notify));
                
                // Process connections from ring buffer
                while (ring_buffer_pop(worker->ring_buffer, &conn)) {
                    handle_client_optimized(conn.client_fd, worker);
                }
            }
        }
        
        // Also try to pop connections without notification (for load balancing)
        while (ring_buffer_pop(worker->ring_buffer, &conn)) {
            handle_client_optimized(conn.client_fd, worker);
        }
    }
    
    return NULL;
}

void handle_client_optimized(int client_fd, worker_thread_t *worker) {
    char buffer[BUFFER_SIZE] __attribute__((aligned(64)));
    ssize_t bytes_read;
    
    // Prefetch buffer
    prefetch_data_inline(buffer);
    
    bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    if (bytes_read <= 0) {
        goto cleanup;
    }
    
    buffer[bytes_read] = '\0';
    
    // Update statistics
    atomic_fetch_add(&worker->stats->bytes_received, bytes_read);
    atomic_fetch_add(&worker->stats->total_requests, 1);
    atomic_fetch_add(&worker->local_requests, 1);
    
    // Fast path request matching
    int endpoint = fast_path_match(buffer, bytes_read);
    
    switch (endpoint) {
        case 0: // GET /
            send_response_optimized(client_fd, &ROOT_RESPONSE, worker->stats);
            atomic_fetch_add(&worker->stats->get_requests, 1);
            atomic_fetch_add(&worker->stats->cache_hits, 1);
            break;
            
        case 1: // GET /stats
            send_stats_response_optimized(client_fd, worker);
            atomic_fetch_add(&worker->stats->stats_requests, 1);
            break;
            
        default: // Unknown endpoint
            send_response_optimized(client_fd, &NOT_FOUND_RESPONSE, worker->stats);
            atomic_fetch_add(&worker->stats->cache_misses, 1);
            break;
    }

cleanup:
    close(client_fd);
    atomic_fetch_sub(&worker->stats->active_connections, 1);
}

void send_response_optimized(int client_fd, const response_t *response, server_stats_t *stats) {
    ssize_t sent = send(client_fd, response->data, response->length, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
        atomic_fetch_add(&stats->bytes_sent, sent);
    }
}

void send_stats_response_optimized(int client_fd, worker_thread_t *worker) {
    char *json_buffer = (char*)allocate_from_pool(worker, 4096);
    time_t current_time = time(NULL);
    double uptime = difftime(current_time, worker->stats->start_time);
    
    // Build comprehensive JSON response
    int len = snprintf(json_buffer, 4096,
        "{\n"
        "  \"server\": \"Ultra-Fast Multi-Core HTTP Server\",\n"
        "  \"version\": \"2.0\",\n"
        "  \"uptime_seconds\": %.0f,\n"
        "  \"performance\": {\n"
        "    \"total_requests\": %lu,\n"
        "    \"get_requests\": %lu,\n"
        "    \"stats_requests\": %lu,\n"
        "    \"requests_per_second\": %lu,\n"
        "    \"bytes_sent\": %lu,\n"
        "    \"bytes_received\": %lu\n"
        "  },\n"
        "  \"connections\": {\n"
        "    \"active\": %lu,\n"
        "    \"peak\": %lu\n"
        "  },\n"
        "  \"system\": {\n"
        "    \"worker_threads\": %d,\n"
        "    \"cpu_cores\": %d,\n"
        "    \"cache_hits\": %lu,\n"
        "    \"cache_misses\": %lu,\n"
        "    \"memory_allocations\": %lu\n"
        "  },\n"
        "  \"errors\": {\n"
        "    \"total_errors\": %lu,\n"
        "    \"error_rate\": %.2f\n"
        "  },\n"
        "  \"thread_stats\": {\n"
        "    \"thread_id\": %d,\n"
        "    \"local_requests\": %lu,\n"
        "    \"local_bytes_sent\": %lu\n"
        "  },\n"
        "  \"timestamp\": \"%s\"\n"
        "}\n",
void send_stats_response_optimized(int client_fd, worker_thread_t *worker) {
    char *json_buffer = (char*)allocate_from_pool(worker, 4096);
    time_t current_time = time(NULL);
    double uptime = difftime(current_time, worker->stats->start_time);
    
    // Build comprehensive JSON response
    int len = snprintf(json_buffer, 4096,
        "{\n"
        "  \"server\": \"Ultra-Fast Multi-Core HTTP Server\",\n"
        "  \"version\": \"2.0\",\n"
        "  \"uptime_seconds\": %.0f,\n"
        "  \"performance\": {\n"
        "    \"total_requests\": %lu,\n"
        "    \"get_requests\": %lu,\n"
        "    \"stats_requests\": %lu,\n"
        "    \"requests_per_second\": %lu,\n"
        "    \"bytes_sent\": %lu,\n"
        "    \"bytes_received\": %lu\n"
        "  },\n"
        "  \"connections\": {\n"
        "    \"active\": %lu,\n"
        "    \"peak\": %lu\n"
        "  },\n"
        "  \"system\": {\n"
        "    \"worker_threads\": %d,\n"
        "    \"cpu_cores\": %ld,\n"
        "    \"cache_hits\": %lu,\n"
        "    \"cache_misses\": %lu,\n"
        "    \"memory_allocations\": %lu\n"
        "  },\n"
        "  \"errors\": {\n"
        "    \"total_errors\": %lu,\n"
        "    \"error_rate\": %.2f\n"
        "  },\n"
        "  \"thread_stats\": {\n"
        "    \"thread_id\": %d,\n"
        "    \"local_requests\": %lu,\n"
        "    \"local_bytes_sent\": %lu\n"
        "  },\n"
        "  \"timestamp\": \"%.24s\"\n"
        "}",
        uptime,
        atomic_load(&worker->stats->total_requests),
        atomic_load(&worker->stats->get_requests),
        atomic_load(&worker->stats->stats_requests),
        atomic_load(&worker->stats->request_rate),
        atomic_load(&worker->stats->bytes_sent),
        atomic_load(&worker->stats->bytes_received),
        atomic_load(&worker->stats->active_connections),
        atomic_load(&worker->stats->peak_connections),
        MAX_WORKER_THREADS,
        sysconf(_SC_NPROCESSORS_ONLN),
        atomic_load(&worker->stats->cache_hits),
        atomic_load(&worker->stats->cache_misses),
        atomic_load(&worker->stats->memory_allocations),
        atomic_load(&worker->stats->error_count),
        uptime > 0 ? (double)atomic_load(&worker->stats->error_count) / atomic_load(&worker->stats->total_requests) * 100 : 0.0,
        worker->thread_id,
        atomic_load(&worker->local_requests),
        atomic_load(&worker->local_bytes_sent),
        ctime(&current_time)
    );
    
    // Send header first
    send_response_optimized(client_fd, &STATS_HEADER, worker->stats);
    
    // Send JSON content
    ssize_t sent = send(client_fd, json_buffer, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent > 0) {
        atomic_fetch_add(&worker->stats->bytes_sent, sent);
    }
}
}