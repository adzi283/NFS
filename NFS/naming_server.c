// naming_server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "common.h"
#include "protocol.h"

#define MAX_STORAGE_SERVERS 100
#define MAX_PATH_LENGTH 1024
#define CACHE_SIZE 20

// Data structure to store Storage Server information
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int nm_port;        // Port for NM Connection
    int client_port;    // Port for Client Connection
    int num_paths;
    char** paths;
    int is_active;
    int ss_fd;          // File descriptor for the Storage Server connection
} StorageServerInfo;

// Data structure for path mapping
typedef struct {
    char path[MAX_PATH_LENGTH];
    StorageServerInfo* ss_info;
} PathMapping;

// LRU Cache Node
typedef struct CacheNode {
    char path[MAX_PATH_LENGTH];
    StorageServerInfo* ss_info;
    struct CacheNode* prev;
    struct CacheNode* next;
} CacheNode;

// Global variables
StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
int storage_server_count = 0;

PathMapping* path_mappings = NULL;
int path_mapping_count = 0;

pthread_mutex_t storage_server_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t path_mapping_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// LRU Cache
CacheNode* cache_head = NULL;
CacheNode* cache_tail = NULL;
int cache_size = 0;

// Function prototypes
void* handle_client(void* arg);
void* handle_storage_server(void* arg);
void* handle_client_request(void* arg);
void* handle_storage_server_messages(void* arg);
StorageServerInfo* find_storage_server_by_path(const char* path);
void update_path_mappings(StorageServerInfo* ss_info);
void add_to_cache(const char* path, StorageServerInfo* ss_info);
StorageServerInfo* get_from_cache(const char* path);
void remove_from_cache(CacheNode* node);
void move_to_front(CacheNode* node);
void log_operation(const char* message);
void remove_storage_server(StorageServerInfo* ss_info);
void remove_paths_by_ss(StorageServerInfo* ss_info);

int main(int argc, char* argv[]) {
    // Modified to accept IP address as an argument
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <IP> <ClientPort> <StorageServerPort>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* ip = argv[1];
    int client_port = atoi(argv[2]);
    int ss_port = atoi(argv[3]);

    // Create listener sockets for clients and storage servers
    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);

    if (client_sock < 0 || ss_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Bind client socket
    struct sockaddr_in client_addr;
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(client_port);
    if (inet_pton(AF_INET, ip, &client_addr.sin_addr) <= 0) {
        perror("Invalid IP address for client socket");
        exit(EXIT_FAILURE);
    }

    if (bind(client_sock, (struct sockaddr*)&client_addr, sizeof(client_addr)) < 0) {
        perror("Failed to bind client socket");
        exit(EXIT_FAILURE);
    }

    // Bind storage server socket
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ip, &ss_addr.sin_addr) <= 0) {
        perror("Invalid IP address for storage server socket");
        exit(EXIT_FAILURE);
    }

    if (bind(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Failed to bind storage server socket");
        exit(EXIT_FAILURE);
    }

    if (listen(client_sock, 10) < 0) {
        perror("Failed to listen on client socket");
        exit(EXIT_FAILURE);
    }

    if (listen(ss_sock, 10) < 0) {
        perror("Failed to listen on storage server socket");
        exit(EXIT_FAILURE);
    }

    printf("Naming Server is running on IP %s, port %d (clients) and port %d (storage servers)\n", ip, client_port, ss_port);

    // Create threads to accept client and storage server connections
    pthread_t client_thread, ss_thread;

    int* client_sock_ptr = malloc(sizeof(int));
    *client_sock_ptr = client_sock;
    pthread_create(&client_thread, NULL, handle_client, client_sock_ptr);

    int* ss_sock_ptr = malloc(sizeof(int));
    *ss_sock_ptr = ss_sock;
    pthread_create(&ss_thread, NULL, handle_storage_server, ss_sock_ptr);

    // Wait for threads to finish (they won't, but this keeps the main thread alive)
    pthread_join(client_thread, NULL);
    pthread_join(ss_thread, NULL);

    close(client_sock);
    close(ss_sock);

    return 0;
}

// Function to handle client connections
void* handle_client(void* arg) {
    int client_sock = *((int*)arg);
    free(arg);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(client_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            perror("Failed to accept client connection");
            continue;
        }

        // Create a new thread to handle the client request
        pthread_t client_request_thread;
        int* client_fd_ptr = malloc(sizeof(int));
        *client_fd_ptr = client_fd;
        pthread_create(&client_request_thread, NULL, handle_client_request, client_fd_ptr);
        pthread_detach(client_request_thread);
    }
    return NULL;
}

// Function to handle individual client requests
void* handle_client_request(void* arg) {
    int client_fd = *((int*)arg);
    free(arg);

    while (1) {
        int32_t opcode_net;
        ssize_t bytes_received = recv(client_fd, &opcode_net, sizeof(opcode_net), 0);
        if (bytes_received <= 0) {
            close(client_fd);
            return NULL;
        }
        int32_t opcode = ntohl(opcode_net);

        // Handle the opcode
        switch (opcode) {
            case OP_READ:
            case OP_WRITE:
            case OP_GET_INFO:
            case OP_STREAM: {
                // Receive path length and path
                int32_t path_len_net;
                if (recv(client_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                    perror("Failed to receive path length");
                    close(client_fd);
                    return NULL;
                }
                int32_t path_len = ntohl(path_len_net);
                if (path_len <= 0 || path_len > MAX_PATH_LENGTH) {
                    perror("Invalid path length");
                    close(client_fd);
                    return NULL;
                }

                char path[MAX_PATH_LENGTH];
                if (recv(client_fd, path, path_len, 0) <= 0) {
                    perror("Failed to receive path");
                    close(client_fd);
                    return NULL;
                }
                path[path_len] = '\0';

                // Additional data for WRITE operation
                int32_t write_mode = 0;
                if (opcode == OP_WRITE) {
                    int32_t write_mode_net;
                    if (recv(client_fd, &write_mode_net, sizeof(write_mode_net), 0) <= 0) {
                        perror("Failed to receive write mode");
                        close(client_fd);
                        return NULL;
                    }
                    write_mode = ntohl(write_mode_net);
                }

                // Find the storage server handling this path
                StorageServerInfo* ss_info = find_storage_server_by_path(path);

                if (ss_info == NULL) {
                    // Send failure status and error message
                    int32_t status_net = htonl(STATUS_FILE_NOT_FOUND);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    const char* error_msg = "File not found";
                    int32_t msg_len = strlen(error_msg);
                    int32_t msg_len_net = htonl(msg_len);
                    send(client_fd, &msg_len_net, sizeof(msg_len_net), 0);
                    send(client_fd, error_msg, msg_len, 0);
                    log_operation("File not found");
                    continue;
                }

                // Send success status
                int32_t status_net = htonl(STATUS_SUCCESS);
                send(client_fd, &status_net, sizeof(status_net), 0);

                // Send storage server IP and port
                int32_t ip_len = strlen(ss_info->ip);
                int32_t ip_len_net = htonl(ip_len);
                int32_t port_net = htonl(ss_info->client_port);
                send(client_fd, &ip_len_net, sizeof(ip_len_net), 0);
                send(client_fd, &port_net, sizeof(port_net), 0);
                send(client_fd, ss_info->ip, ip_len, 0);

                log_operation("Sent storage server info to client");

                break;
            }
            case OP_CREATE_FILE:
            case OP_CREATE_DIR:
            case OP_DELETE_FILE:
            case OP_DELETE_DIR: {
                // Receive path length and path
                int32_t path_len_net;
                if (recv(client_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                    perror("Failed to receive path length");
                    close(client_fd);
                    return NULL;
                }
                int32_t path_len = ntohl(path_len_net);
                if (path_len <= 0 || path_len > MAX_PATH_LENGTH) {
                    perror("Invalid path length");
                    close(client_fd);
                    return NULL;
                }

                char path[MAX_PATH_LENGTH];
                if (recv(client_fd, path, path_len, 0) <= 0) {
                    perror("Failed to receive path");
                    close(client_fd);
                    return NULL;
                }
                path[path_len] = '\0';

                // For simplicity, assign the operation to a storage server (e.g., first one)
                pthread_mutex_lock(&storage_server_mutex);
                if (storage_server_count == 0) {
                    // No storage servers available
                    pthread_mutex_unlock(&storage_server_mutex);
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    log_operation("No storage servers available");
                    continue;
                }
                StorageServerInfo* ss_info = &storage_servers[0];
                pthread_mutex_unlock(&storage_server_mutex);

                // Send request to storage server
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_sock < 0) {
                    perror("Socket creation failed");
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    continue;
                }

                struct sockaddr_in ss_addr;
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(ss_info->nm_port);
                if (inet_pton(AF_INET, ss_info->ip, &ss_addr.sin_addr) <= 0) {
                    perror("Invalid SS IP address");
                    close(ss_sock);
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    continue;
                }

                if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
                    perror("Connection to Storage Server failed");
                    close(ss_sock);
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    continue;
                }

                // Send opcode and path to SS
                int32_t ss_opcode_net = htonl(opcode);
                send(ss_sock, &ss_opcode_net, sizeof(ss_opcode_net), 0);
                send(ss_sock, &path_len_net, sizeof(path_len_net), 0);
                send(ss_sock, path, path_len, 0);

                // Receive acknowledgment from SS
                int32_t ss_status_net;
                if (recv(ss_sock, &ss_status_net, sizeof(ss_status_net), 0) <= 0) {
                    perror("Failed to receive status from Storage Server");
                    close(ss_sock);
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    continue;
                }
                int32_t ss_status = ntohl(ss_status_net);

                if (ss_status == STATUS_SUCCESS) {
                    // Update path mappings
                    pthread_mutex_lock(&path_mapping_mutex);
                    if (opcode == OP_CREATE_FILE || opcode == OP_CREATE_DIR) {
                        // Add path to path_mappings
                        path_mappings = realloc(path_mappings, (path_mapping_count + 1) * sizeof(PathMapping));
                        strcpy(path_mappings[path_mapping_count].path, path);
                        path_mappings[path_mapping_count].ss_info = ss_info;
                        path_mapping_count++;
                    } else if (opcode == OP_DELETE_FILE || opcode == OP_DELETE_DIR) {
                        // Remove path from path_mappings
                        for (int i = 0; i < path_mapping_count; ++i) {
                            if (strcmp(path_mappings[i].path, path) == 0) {
                                // Remove from cache if present
                                pthread_mutex_lock(&cache_mutex);
                                CacheNode* node = cache_head;
                                while (node != NULL) {
                                    if (strcmp(node->path, path) == 0) {
                                        CacheNode* temp = node->next; // Save next node
                                        remove_from_cache(node);
                                        node = temp;
                                    } else {
                                        node = node->next;
                                    }
                                }
                                pthread_mutex_unlock(&cache_mutex);

                                // Remove from path_mappings
                                if (i < path_mapping_count - 1) {
                                    memmove(&path_mappings[i], &path_mappings[i + 1], (path_mapping_count - i - 1) * sizeof(PathMapping));
                                }
                                path_mapping_count--;
                                if (path_mapping_count == 0) {
                                    free(path_mappings);
                                    path_mappings = NULL;
                                } else {
                                    path_mappings = realloc(path_mappings, path_mapping_count * sizeof(PathMapping));
                                }
                                break;
                            }
                        }
                    }
                    pthread_mutex_unlock(&path_mapping_mutex);

                    // Send success to client
                    int32_t status_net = htonl(STATUS_SUCCESS);
                    send(client_fd, &status_net, sizeof(status_net), 0);

                    log_operation("Operation successful");
                } else {
                    // Send failure to client
                    int32_t status_net = htonl(STATUS_FAILURE);
                    send(client_fd, &status_net, sizeof(status_net), 0);

                    log_operation("Operation failed");
                }

                close(ss_sock);
                break;
            }
            case OP_LIST: {
                // Send success status
                int32_t status_net = htonl(STATUS_SUCCESS);
                send(client_fd, &status_net, sizeof(status_net), 0);

                // Send number of paths
                pthread_mutex_lock(&path_mapping_mutex);
                int32_t num_paths_net = htonl(path_mapping_count);
                send(client_fd, &num_paths_net, sizeof(num_paths_net), 0);

                // Send each path
                for (int i = 0; i < path_mapping_count; ++i) {
                    int32_t path_len = strlen(path_mappings[i].path);
                    int32_t path_len_net = htonl(path_len);
                    send(client_fd, &path_len_net, sizeof(path_len_net), 0);
                    send(client_fd, path_mappings[i].path, path_len, 0);
                }
                pthread_mutex_unlock(&path_mapping_mutex);

                log_operation("Sent accessible paths to client");
                break;
            }
            default:
                // Invalid operation
                int32_t status_net = htonl(STATUS_INVALID_OPERATION);
                send(client_fd, &status_net, sizeof(status_net), 0);
                log_operation("Invalid operation code from client");
                break;
        }
    }

    close(client_fd);
    return NULL;
}

// Function to handle storage server connections
void* handle_storage_server(void* arg) {
    int ss_sock = *((int*)arg);
    free(arg);

    while (1) {
        struct sockaddr_in ss_addr;
        socklen_t addr_len = sizeof(ss_addr);
        int ss_fd = accept(ss_sock, (struct sockaddr*)&ss_addr, &addr_len);
        if (ss_fd < 0) {
            perror("Failed to accept storage server connection");
            continue;
        }

        // Handle storage server registration
        // Receive StorageServerInfo from SS
        StorageServerInfo ss_info;
        memset(&ss_info, 0, sizeof(ss_info));
        ss_info.is_active = 1;
        ss_info.ss_fd = ss_fd; // Store the file descriptor

        // Receive IP length and IP
        int32_t ip_len_net;
        if (recv(ss_fd, &ip_len_net, sizeof(ip_len_net), 0) <= 0) {
            perror("Failed to receive IP length");
            close(ss_fd);
            continue;
        }
        int32_t ip_len = ntohl(ip_len_net);
        if (ip_len <= 0 || ip_len >= INET_ADDRSTRLEN) {
            perror("Invalid IP length");
            close(ss_fd);
            continue;
        }
        if (recv(ss_fd, ss_info.ip, ip_len, 0) <= 0) {
            perror("Failed to receive IP");
            close(ss_fd);
            continue;
        }
        ss_info.ip[ip_len] = '\0';

        // Receive NM port and client port
        int32_t nm_port_net, client_port_net;
        if (recv(ss_fd, &nm_port_net, sizeof(nm_port_net), 0) <= 0 ||
            recv(ss_fd, &client_port_net, sizeof(client_port_net), 0) <= 0) {
            perror("Failed to receive ports");
            close(ss_fd);
            continue;
        }
        ss_info.nm_port = ntohl(nm_port_net);
        ss_info.client_port = ntohl(client_port_net);

        // Receive number of paths
        int32_t num_paths_net;
        if (recv(ss_fd, &num_paths_net, sizeof(num_paths_net), 0) <= 0) {
            perror("Failed to receive number of paths");
            close(ss_fd);
            continue;
        }
        ss_info.num_paths = ntohl(num_paths_net);

        // Receive each path
        ss_info.paths = malloc(ss_info.num_paths * sizeof(char*));
        for (int i = 0; i < ss_info.num_paths; ++i) {
            int32_t path_len_net;
            if (recv(ss_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                perror("Failed to receive path length");
                close(ss_fd);
                continue;
            }
            int32_t path_len = ntohl(path_len_net);
            if (path_len <= 0 || path_len > MAX_PATH_LENGTH) {
                perror("Invalid path length");
                close(ss_fd);
                continue;
            }
            ss_info.paths[i] = malloc(path_len + 1);
            if (recv(ss_fd, ss_info.paths[i], path_len, 0) <= 0) {
                perror("Failed to receive path");
                close(ss_fd);
                continue;
            }
            ss_info.paths[i][path_len] = '\0';
        }

        // Add storage server info to list
        pthread_mutex_lock(&storage_server_mutex);
        if (storage_server_count < MAX_STORAGE_SERVERS) {
            storage_servers[storage_server_count] = ss_info;
            storage_server_count++;

            // Update path mappings
            update_path_mappings(&storage_servers[storage_server_count - 1]);

            log_operation("Storage server registered successfully");
        } else {
            // Max storage servers reached
            log_operation("Max storage servers reached");
        }
        pthread_mutex_unlock(&storage_server_mutex);

        // Send acknowledgment to storage server
        int32_t status_net = htonl(STATUS_SUCCESS);
        send(ss_fd, &status_net, sizeof(status_net), 0);

        // Create a thread to handle messages from the storage server
        pthread_t ss_msg_thread;
        StorageServerInfo* ss_info_ptr = &storage_servers[storage_server_count - 1];
        pthread_create(&ss_msg_thread, NULL, handle_storage_server_messages, ss_info_ptr);
        pthread_detach(ss_msg_thread);
    }
    return NULL;
}

// Function to handle messages from storage servers
void* handle_storage_server_messages(void* arg) {
    StorageServerInfo* ss_info = (StorageServerInfo*)arg;
    int ss_fd = ss_info->ss_fd;

    while (1) {
        int32_t opcode_net;
        ssize_t bytes_received = recv(ss_fd, &opcode_net, sizeof(opcode_net), 0);
        if (bytes_received <= 0) {
            perror("Storage Server connection lost");
            // Handle storage server disconnection
            pthread_mutex_lock(&storage_server_mutex);
            ss_info->is_active = 0;
            pthread_mutex_unlock(&storage_server_mutex);

            remove_paths_by_ss(ss_info);
            close(ss_fd);
            break;
        }
        int32_t opcode = ntohl(opcode_net);

        switch (opcode) {
            case OP_HEARTBEAT:
                // Handle heartbeat (optional)
                // printf("Received heartbeat from Storage Server\n");
                break;

            case OP_ASYNC_WRITE_COMPLETE: {
                // Receive path length and path
                int32_t path_len_net;
                if (recv(ss_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                    perror("Failed to receive path length");
                    close(ss_fd);
                    break;
                }
                int32_t path_len = ntohl(path_len_net);
                char path[MAX_PATH_LENGTH];
                if (recv(ss_fd, path, path_len, 0) <= 0) {
                    perror("Failed to receive path");
                    close(ss_fd);
                    break;
                }
                path[path_len] = '\0';

                // Receive status
                int32_t status_net;
                if (recv(ss_fd, &status_net, sizeof(status_net), 0) <= 0) {
                    perror("Failed to receive status");
                    close(ss_fd);
                    break;
                }
                int32_t status = ntohl(status_net);

                // Log the completion
                if (status == STATUS_SUCCESS) {
                    char message[256];
                    snprintf(message, sizeof(message), "Asynchronous write completed successfully for path: %s", path);
                    log_operation(message);
                } else {
                    char message[256];
                    snprintf(message, sizeof(message), "Asynchronous write failed for path: %s", path);
                    log_operation(message);
                }

                break;
            }

            default:
                printf("Unknown opcode received from Storage Server: %d\n", opcode);
                break;
        }
    }

    return NULL;
}

// Function to remove paths associated with a storage server
void remove_paths_by_ss(StorageServerInfo* ss_info) {
    pthread_mutex_lock(&path_mapping_mutex);
    for (int i = 0; i < path_mapping_count; ) {
        if (path_mappings[i].ss_info == ss_info) {
            // Remove from cache if present
            pthread_mutex_lock(&cache_mutex);
            CacheNode* node = cache_head;
            while (node != NULL) {
                if (strcmp(node->path, path_mappings[i].path) == 0) {
                    CacheNode* temp = node->next; // Save next node
                    remove_from_cache(node);
                    node = temp;
                } else {
                    node = node->next;
                }
            }
            pthread_mutex_unlock(&cache_mutex);

            // Remove from path_mappings
            if (i < path_mapping_count - 1) {
                memmove(&path_mappings[i], &path_mappings[i + 1], (path_mapping_count - i - 1) * sizeof(PathMapping));
            }
            path_mapping_count--;
            if (path_mapping_count == 0) {
                free(path_mappings);
                path_mappings = NULL;
                break;
            } else {
                path_mappings = realloc(path_mappings, path_mapping_count * sizeof(PathMapping));
            }
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&path_mapping_mutex);
}

// Function to find the storage server handling a given path
StorageServerInfo* find_storage_server_by_path(const char* path) {
    // First, check the cache
    pthread_mutex_lock(&cache_mutex);
    StorageServerInfo* ss_info = get_from_cache(path);
    if (ss_info != NULL) {
        pthread_mutex_unlock(&cache_mutex);
        return ss_info;
    }
    pthread_mutex_unlock(&cache_mutex);

    // Search in path mappings
    pthread_mutex_lock(&path_mapping_mutex);
    if (path_mapping_count == 0 || path_mappings == NULL) {
        pthread_mutex_unlock(&path_mapping_mutex);
        return NULL;
    }
    for (int i = 0; i < path_mapping_count; ++i) {
        if (strcmp(path_mappings[i].path, path) == 0) {
            ss_info = path_mappings[i].ss_info;

            // Add to cache
            pthread_mutex_lock(&cache_mutex);
            add_to_cache(path, ss_info);
            pthread_mutex_unlock(&cache_mutex);

            pthread_mutex_unlock(&path_mapping_mutex);
            return ss_info;
        }
    }
    pthread_mutex_unlock(&path_mapping_mutex);
    return NULL;
}

// Function to update path mappings when a new storage server registers
void update_path_mappings(StorageServerInfo* ss_info) {
    pthread_mutex_lock(&path_mapping_mutex);
    for (int i = 0; i < ss_info->num_paths; ++i) {
        path_mappings = realloc(path_mappings, (path_mapping_count + 1) * sizeof(PathMapping));
        strcpy(path_mappings[path_mapping_count].path, ss_info->paths[i]);
        path_mappings[path_mapping_count].ss_info = ss_info;
        path_mapping_count++;
    }
    pthread_mutex_unlock(&path_mapping_mutex);
}

// LRU Cache functions
void add_to_cache(const char* path, StorageServerInfo* ss_info) {
    // Check if already in cache
    CacheNode* node = cache_head;
    while (node != NULL) {
        if (strcmp(node->path, path) == 0) {
            // Move to front
            move_to_front(node);
            return;
        }
        node = node->next;
    }

    // Create new node
    CacheNode* new_node = malloc(sizeof(CacheNode));
    strcpy(new_node->path, path);
    new_node->ss_info = ss_info;
    new_node->prev = NULL;
    new_node->next = cache_head;

    if (cache_head != NULL) {
        cache_head->prev = new_node;
    }
    cache_head = new_node;

    if (cache_tail == NULL) {
        cache_tail = new_node;
    }

    cache_size++;
    if (cache_size > CACHE_SIZE) {
        // Remove least recently used node
        remove_from_cache(cache_tail);
    }
}

StorageServerInfo* get_from_cache(const char* path) {
    CacheNode* node = cache_head;
    while (node != NULL) {
        if (strcmp(node->path, path) == 0) {
            // Move to front
            move_to_front(node);
            return node->ss_info;
        }
        node = node->next;
    }
    return NULL;
}

void remove_from_cache(CacheNode* node) {
    if (node == NULL) return;

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        cache_head = node->next;
    }

    if (node->next != NULL) {
        node->next->prev = node->prev;
    } else {
        cache_tail = node->prev;
    }

    free(node);
    cache_size--;
}

void move_to_front(CacheNode* node) {
    if (node == cache_head) return;

    // Remove node
    if (node->prev != NULL) {
        node->prev->next = node->next;
    }
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    if (node == cache_tail) {
        cache_tail = node->prev;
    }

    // Move to front
    node->prev = NULL;
    node->next = cache_head;
    if (cache_head != NULL) {
        cache_head->prev = node;
    }
    cache_head = node;
    if (cache_tail == NULL) {
        cache_tail = node;
    }
}

// Logging function
void log_operation(const char* message) {
    // For simplicity, just print to stdout
    printf("[LOG] %s\n", message);
}
