// storage_server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h> // For gethostbyname
#include "common.h"
#include "protocol.h"

#define MAX_PATH_LENGTH 1024
#define BUFFER_SIZE 1024

// Global variables
char nm_ip_global[INET_ADDRSTRLEN];
int nm_port_global;
char storage_dir[MAX_PATH_LENGTH]; // Added global variable for storage directory

// Function prototypes
void* handle_client(void* arg);
void* handle_client_request(void* arg);
void* handle_nm_request(void* arg);
void* process_async_write(void* arg);
void register_with_naming_server(const char* nm_ip, int nm_port, int ss_nm_port, int ss_client_port, char** paths, int num_paths);
void send_async_write_completion(const char* nm_ip, int nm_port, const char* path, int status);
void ensure_directory_exists(const char* path); // Added function prototype

// Data structures
typedef struct {
    int client_sock;
    struct sockaddr_in client_addr;
} ClientInfo;

typedef struct {
    int nm_sock;
    struct sockaddr_in nm_addr;
} NMInfo;

typedef struct {
    char path[MAX_PATH_LENGTH];
    char* data;
    char nm_ip[INET_ADDRSTRLEN];
    int nm_port;
    int status;
} AsyncWriteInfo;

int main(int argc, char* argv[]) {
    // Modified argument parsing to handle the root path '/' and storage directory './storage'
    if (argc < 6) {
        fprintf(stderr, "Usage: %s <NamingServerIP> <NamingServerPort> <SS_NamingPort> <SS_ClientPort> <Paths...> <StorageDir>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* nm_ip = argv[1];
    int nm_port = atoi(argv[2]);
    int ss_nm_port = atoi(argv[3]);
    int ss_client_port = atoi(argv[4]);

    // The storage directory is always the last argument
    strncpy(storage_dir, argv[argc - 1], MAX_PATH_LENGTH);

    int num_paths = argc - 6; // Exclude the first 5 arguments and storage_dir
    char** paths = NULL;

    if (num_paths > 0) {
        paths = &argv[5];
    } else {
        // No paths specified, set num_paths to 1 and paths[0] to "/"
        num_paths = 1;
        paths = malloc(sizeof(char*));
        paths[0] = "/";
    }

    // Store NM IP and port globally
    strcpy(nm_ip_global, nm_ip);
    nm_port_global = nm_port; // Assuming nm_port is the NM port for storage server

    // Register with Naming Server
    register_with_naming_server(nm_ip_global, nm_port_global, ss_nm_port, ss_client_port, paths, num_paths);

    // Create socket to listen for client connections
    int ss_client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_client_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in ss_client_addr;
    ss_client_addr.sin_family = AF_INET;
    ss_client_addr.sin_port = htons(ss_client_port);
    ss_client_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ss_client_sock, (struct sockaddr*)&ss_client_addr, sizeof(ss_client_addr)) < 0) {
        perror("Failed to bind storage server client socket");
        close(ss_client_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(ss_client_sock, 10) < 0) {
        perror("Failed to listen on storage server client socket");
        close(ss_client_sock);
        exit(EXIT_FAILURE);
    }

    // Create socket to listen for NM connections
    int ss_nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_nm_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in ss_nm_addr;
    ss_nm_addr.sin_family = AF_INET;
    ss_nm_addr.sin_port = htons(ss_nm_port);
    ss_nm_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ss_nm_sock, (struct sockaddr*)&ss_nm_addr, sizeof(ss_nm_addr)) < 0) {
        perror("Failed to bind storage server NM socket");
        close(ss_nm_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(ss_nm_sock, 10) < 0) {
        perror("Failed to listen on storage server NM socket");
        close(ss_nm_sock);
        exit(EXIT_FAILURE);
    }

    printf("Storage Server is listening on port %d for clients and port %d for NM\n", ss_client_port, ss_nm_port);

    // Create threads to accept client and NM connections
    pthread_t client_thread, nm_thread;

    int* ss_client_sock_ptr = malloc(sizeof(int));
    *ss_client_sock_ptr = ss_client_sock;
    pthread_create(&client_thread, NULL, handle_client, ss_client_sock_ptr);

    int* ss_nm_sock_ptr = malloc(sizeof(int));
    *ss_nm_sock_ptr = ss_nm_sock;
    pthread_create(&nm_thread, NULL, handle_nm_request, ss_nm_sock_ptr);

    // Wait for threads to finish (they won't, but this keeps the main thread alive)
    pthread_join(client_thread, NULL);
    pthread_join(nm_thread, NULL);

    close(ss_client_sock);
    close(ss_nm_sock);

    return 0;
}

// Function to ensure directory exists
void ensure_directory_exists(const char* path) {
    char dir_path[MAX_PATH_LENGTH];
    strncpy(dir_path, path, MAX_PATH_LENGTH);
    char* last_slash = strrchr(dir_path, '/');
    if (last_slash != NULL) {
        *last_slash = '\0'; // Remove filename to get directory path
        // Create directories recursively
        char command[MAX_PATH_LENGTH + 10];
        snprintf(command, sizeof(command), "mkdir -p \"%s\"", dir_path);
        system(command);
    }
}

// Function to register with the Naming Server
void register_with_naming_server(const char* nm_ip, int nm_port, int ss_nm_port, int ss_client_port, char** paths, int num_paths) {
    int nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(nm_port);
    if (inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr) <= 0) {
        perror("Invalid Naming Server IP address");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    if (connect(nm_sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Connection to Naming Server failed");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Send registration information
    // Send IP length and IP
    char ss_ip[INET_ADDRSTRLEN];
    gethostname(ss_ip, INET_ADDRSTRLEN);
    struct hostent* he = gethostbyname(ss_ip);
    if (he == NULL) {
        perror("Failed to get host IP");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }
    struct in_addr** addr_list = (struct in_addr**)he->h_addr_list;
    strcpy(ss_ip, inet_ntoa(*addr_list[0]));
    int32_t ip_len = strlen(ss_ip);
    int32_t ip_len_net = htonl(ip_len);
    if (send(nm_sock, &ip_len_net, sizeof(ip_len_net), 0) < 0 ||
        send(nm_sock, ss_ip, ip_len, 0) < 0) {
        perror("Failed to send IP to Naming Server");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Send NM port and client port
    int32_t ss_nm_port_net = htonl(ss_nm_port);
    int32_t ss_client_port_net = htonl(ss_client_port);
    if (send(nm_sock, &ss_nm_port_net, sizeof(ss_nm_port_net), 0) < 0 ||
        send(nm_sock, &ss_client_port_net, sizeof(ss_client_port_net), 0) < 0) {
        perror("Failed to send ports to Naming Server");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Send number of paths
    int32_t num_paths_net = htonl(num_paths);
    if (send(nm_sock, &num_paths_net, sizeof(num_paths_net), 0) < 0) {
        perror("Failed to send number of paths to Naming Server");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    // Send each path
    for (int i = 0; i < num_paths; ++i) {
        int32_t path_len = strlen(paths[i]);
        int32_t path_len_net = htonl(path_len);
        if (send(nm_sock, &path_len_net, sizeof(path_len_net), 0) < 0 ||
            send(nm_sock, paths[i], path_len, 0) < 0) {
            perror("Failed to send path to Naming Server");
            close(nm_sock);
            exit(EXIT_FAILURE);
        }
    }

    // Receive acknowledgment from Naming Server
    int32_t status_net;
    if (recv(nm_sock, &status_net, sizeof(status_net), 0) <= 0) {
        perror("Failed to receive acknowledgment from Naming Server");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }
    int32_t status = ntohl(status_net);

    if (status != STATUS_SUCCESS) {
        printf("Failed to register with Naming Server.\n");
        close(nm_sock);
        exit(EXIT_FAILURE);
    }

    printf("Registered with Naming Server successfully.\n");

    // For simplicity, we'll close the socket here
    close(nm_sock);
}

// ... Rest of the code remains unchanged


// Function to handle client connections
void* handle_client(void* arg) {
    int ss_client_sock = *((int*)arg);
    free(arg);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(ss_client_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            perror("Failed to accept client connection");
            continue;
        }

        // Create a new thread to handle the client request
        pthread_t client_request_thread;
        ClientInfo* client_info = malloc(sizeof(ClientInfo));
        client_info->client_sock = client_fd;
        client_info->client_addr = client_addr;
        pthread_create(&client_request_thread, NULL, handle_client_request, client_info);
        pthread_detach(client_request_thread);
    }
    return NULL;
}

// Function to handle individual client requests
void* handle_client_request(void* arg) {
    ClientInfo* client_info = (ClientInfo*)arg;
    int client_fd = client_info->client_sock;
    free(client_info);

    while (1) {
        int32_t opcode_net;
        ssize_t bytes_received = recv(client_fd, &opcode_net, sizeof(opcode_net), 0);
        if (bytes_received <= 0) {
            close(client_fd);
            return NULL;
        }
        int32_t opcode = ntohl(opcode_net);

        switch (opcode) {
            case OP_READ: {
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

                // Adjust full path
                char full_path[MAX_PATH_LENGTH];
                snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", storage_dir, path);

                // Read file content
                FILE* file = fopen(full_path, "rb");
                if (!file) {
                    // Send negative file size to indicate error
                    int32_t file_size_net = htonl(-1);
                    send(client_fd, &file_size_net, sizeof(file_size_net), 0);
                    perror("Failed to open file for reading");
                    continue;
                }

                // Get file size
                fseek(file, 0, SEEK_END);
                int32_t file_size = ftell(file);
                fseek(file, 0, SEEK_SET);

                int32_t file_size_net = htonl(file_size);
                if (send(client_fd, &file_size_net, sizeof(file_size_net), 0) < 0) {
                    perror("Failed to send file size");
                    fclose(file);
                    close(client_fd);
                    return NULL;
                }

                // Send file content
                char buffer[BUFFER_SIZE];
                int bytes_read;
                while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
                    if (send(client_fd, buffer, bytes_read, 0) < 0) {
                        perror("Failed to send file content");
                        break;
                    }
                }

                fclose(file);
                break;
            }
            case OP_WRITE: {
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

                // Adjust full path
                char full_path[MAX_PATH_LENGTH];
                snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", storage_dir, path);

                // Receive write mode
                int32_t write_mode_net;
                if (recv(client_fd, &write_mode_net, sizeof(write_mode_net), 0) <= 0) {
                    perror("Failed to receive write mode");
                    close(client_fd);
                    return NULL;
                }
                int32_t write_mode = ntohl(write_mode_net);

                // Receive data length
                int32_t data_len_net;
                if (recv(client_fd, &data_len_net, sizeof(data_len_net), 0) <= 0) {
                    perror("Failed to receive data length");
                    close(client_fd);
                    return NULL;
                }
                int32_t data_len = ntohl(data_len_net);

                // Receive data
                char* data = malloc(data_len + 1);
                int bytes_received = 0;
                while (bytes_received < data_len) {
                    int chunk = recv(client_fd, data + bytes_received, data_len - bytes_received, 0);
                    if (chunk <= 0) {
                        perror("Failed to receive data");
                        free(data);
                        close(client_fd);
                        return NULL;
                    }
                    bytes_received += chunk;
                }
                data[data_len] = '\0';

                // Ensure directories exist
                ensure_directory_exists(full_path);

                if (write_mode == WRITE_MODE_SYNC) {
                    // Write data to file synchronously
                    FILE* file = fopen(full_path, "wb");
                    if (!file) {
                        perror("Failed to open file for writing");
                        int32_t status_net = htonl(STATUS_FAILURE);
                        send(client_fd, &status_net, sizeof(status_net), 0);
                        free(data);
                        continue;
                    }

                    if (fwrite(data, 1, data_len, file) != data_len) {
                        perror("Failed to write data to file");
                        int32_t status_net = htonl(STATUS_FAILURE);
                        send(client_fd, &status_net, sizeof(status_net), 0);
                        free(data);
                        fclose(file);
                        continue;
                    }

                    fclose(file);
                    int32_t status_net = htonl(STATUS_SUCCESS);
                    send(client_fd, &status_net, sizeof(status_net), 0);
                    printf("Data written to %s (synchronous)\n", full_path);
                    free(data);
                } else if (write_mode == WRITE_MODE_ASYNC) {
                    // Handle asynchronous write
                    int32_t status_net = htonl(STATUS_SUCCESS);
                    send(client_fd, &status_net, sizeof(status_net), 0);

                    // Prepare AsyncWriteInfo
                    AsyncWriteInfo* async_info = malloc(sizeof(AsyncWriteInfo));
                    strcpy(async_info->path, full_path);
                    async_info->data = data; // Transfer ownership of data
                    strcpy(async_info->nm_ip, nm_ip_global);
                    async_info->nm_port = nm_port_global;
                    async_info->status = STATUS_SUCCESS;

                    // Create a thread to handle the async write
                    pthread_t async_thread;
                    pthread_create(&async_thread, NULL, process_async_write, async_info);
                    pthread_detach(async_thread);
                }

                break;
            }
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

                // Adjust full path
                char full_path[MAX_PATH_LENGTH];
                snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", storage_dir, path);

                // Open audio file
                FILE* file = fopen(full_path, "rb");
                if (!file) {
                    perror("Failed to open audio file");
                    close(client_fd);
                    return NULL;
                }

                // Stream audio data
                char buffer[BUFFER_SIZE];
                int bytes_read;
                while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
                    if (send(client_fd, buffer, bytes_read, 0) < 0) {
                        perror("Failed to send audio data");
                        break;
                    }
                }

                fclose(file);
                break;
            }
            default:
                // Invalid operation
                printf("Invalid operation code received from client.\n");
                break;
        }
    }

    close(client_fd);
    return NULL;
}

// Function to handle NM requests
void* handle_nm_request(void* arg) {
    int ss_nm_sock = *((int*)arg);
    free(arg);

    while (1) {
        struct sockaddr_in nm_addr;
        socklen_t addr_len = sizeof(nm_addr);
        int nm_fd = accept(ss_nm_sock, (struct sockaddr*)&nm_addr, &addr_len);
        if (nm_fd < 0) {
            perror("Failed to accept NM connection");
            continue;
        }

        // Handle NM requests
        while (1) {
            int32_t opcode_net;
            ssize_t bytes_received = recv(nm_fd, &opcode_net, sizeof(opcode_net), 0);
            if (bytes_received <= 0) {
                close(nm_fd);
                break;
            }
            int32_t opcode = ntohl(opcode_net);

            switch (opcode) {
                case OP_CREATE_FILE:
                case OP_CREATE_DIR: {
                    // Receive path length and path
                    int32_t path_len_net;
                    if (recv(nm_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                        perror("Failed to receive path length");
                        close(nm_fd);
                        return NULL;
                    }
                    int32_t path_len = ntohl(path_len_net);
                    if (path_len <= 0 || path_len > MAX_PATH_LENGTH) {
                        perror("Invalid path length");
                        close(nm_fd);
                        return NULL;
                    }

                    char path[MAX_PATH_LENGTH];
                    if (recv(nm_fd, path, path_len, 0) <= 0) {
                        perror("Failed to receive path");
                        close(nm_fd);
                        return NULL;
                    }
                    path[path_len] = '\0';

                    // Adjust full path
                    char full_path[MAX_PATH_LENGTH];
                    snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", storage_dir, path);

                    // Ensure directories exist
                    ensure_directory_exists(full_path);

                    int status = STATUS_FAILURE;
                    if (opcode == OP_CREATE_FILE) {
                        FILE* file = fopen(full_path, "wb");
                        if (file) {
                            fclose(file);
                            status = STATUS_SUCCESS;
                        } else {
                            perror("Failed to create file");
                        }
                    } else if (opcode == OP_CREATE_DIR) {
                        if (mkdir(full_path, 0777) == 0) {
                            status = STATUS_SUCCESS;
                        } else {
                            perror("Failed to create directory");
                        }
                    }

                    // Send acknowledgment to NM
                    int32_t status_net = htonl(status);
                    send(nm_fd, &status_net, sizeof(status_net), 0);

                    break;
                }
                case OP_DELETE_FILE:
                case OP_DELETE_DIR: {
                    // Receive path length and path
                    int32_t path_len_net;
                    if (recv(nm_fd, &path_len_net, sizeof(path_len_net), 0) <= 0) {
                        perror("Failed to receive path length");
                        close(nm_fd);
                        return NULL;
                    }
                    int32_t path_len = ntohl(path_len_net);
                    if (path_len <= 0 || path_len > MAX_PATH_LENGTH) {
                        perror("Invalid path length");
                        close(nm_fd);
                        return NULL;
                    }

                    char path[MAX_PATH_LENGTH];
                    if (recv(nm_fd, path, path_len, 0) <= 0) {
                        perror("Failed to receive path");
                        close(nm_fd);
                        return NULL;
                    }
                    path[path_len] = '\0';

                    // Adjust full path
                    char full_path[MAX_PATH_LENGTH];
                    snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", storage_dir, path);

                    int status = STATUS_FAILURE;
                    if (opcode == OP_DELETE_FILE) {
                        if (remove(full_path) == 0) {
                            status = STATUS_SUCCESS;
                        } else {
                            perror("Failed to delete file");
                        }
                    } else if (opcode == OP_DELETE_DIR) {
                        if (rmdir(full_path) == 0) {
                            status = STATUS_SUCCESS;
                        } else {
                            perror("Failed to delete directory");
                        }
                    }

                    // Send acknowledgment to NM
                    int32_t status_net = htonl(status);
                    send(nm_fd, &status_net, sizeof(status_net), 0);

                    break;
                }
                default:
                    printf("Invalid operation code received from NM.\n");
                    break;
            }
        }
    }
    return NULL;
}

// Function to process asynchronous write
void* process_async_write(void* arg) {
    AsyncWriteInfo* async_info = (AsyncWriteInfo*)arg;

    // Simulate some delay
    sleep(5);

    // Ensure directories exist
    ensure_directory_exists(async_info->path);

    // Write data to file
    FILE* file = fopen(async_info->path, "wb");
    if (!file) {
        perror("Failed to open file for writing");
        async_info->status = STATUS_FAILURE;
    } else {
        if (fwrite(async_info->data, 1, strlen(async_info->data), file) != strlen(async_info->data)) {
            perror("Failed to write data to file");
            async_info->status = STATUS_FAILURE;
        }
        fclose(file);
    }

    printf("Data written to %s (asynchronous)\n", async_info->path);

    // Notify Naming Server about completion
    // Use global NM IP and port
    send_async_write_completion(nm_ip_global, nm_port_global, async_info->path, async_info->status);

    free(async_info->data);
    free(async_info);
    return NULL;
}

// Function to notify Naming Server about asynchronous write completion
void send_async_write_completion(const char* nm_ip, int nm_port, const char* path, int status) {
    int nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) {
        perror("Socket creation failed");
        return;
    }

    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(nm_port);
    if (inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr) <= 0) {
        perror("Invalid Naming Server IP address");
        close(nm_sock);
        return;
    }

    if (connect(nm_sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Connection to Naming Server failed");
        close(nm_sock);
        return;
    }

    // Send ASYNC_WRITE_COMPLETE opcode
    int32_t opcode_net = htonl(OP_ASYNC_WRITE_COMPLETE);
    if (send(nm_sock, &opcode_net, sizeof(opcode_net), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = strlen(path);
    int32_t path_len_net = htonl(path_len);
    if (send(nm_sock, &path_len_net, sizeof(path_len_net), 0) < 0 ||
        send(nm_sock, path, path_len, 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Send status
    int32_t status_net = htonl(status);
    if (send(nm_sock, &status_net, sizeof(status_net), 0) < 0) {
        perror("Failed to send status to Naming Server");
        close(nm_sock);
        return;
    }

    printf("Asynchronous write completion notification sent to Naming Server for path %s\n", path);

    close(nm_sock);
}
