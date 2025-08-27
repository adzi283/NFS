// client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "common.h"
#include "protocol.h"

#define BUFFER_SIZE 1024

void handle_read(const char* nm_ip, int nm_port);
void handle_write(const char* nm_ip, int nm_port);
void handle_create(const char* nm_ip, int nm_port, int is_directory);
void handle_delete(const char* nm_ip, int nm_port, int is_directory);
void handle_list(const char* nm_ip, int nm_port);
void handle_get_info(const char* nm_ip, int nm_port);
void handle_stream(const char* nm_ip, int nm_port);

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <NamingServerIP> <NamingServerPort>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* nm_ip = argv[1];
    int nm_port = atoi(argv[2]);

    while (1) {
        printf("\nAvailable commands:\n");
        printf("READ\nWRITE\nCREATE FILE\nCREATE DIR\nDELETE FILE\nDELETE DIR\nLIST\nGET_INFO\nSTREAM\n");
        printf("Enter command: ");

        char command[BUFFER_SIZE];
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }

        // Remove trailing newline
        command[strcspn(command, "\n")] = '\0';

        if (strcasecmp(command, "READ") == 0) {
            handle_read(nm_ip, nm_port);
        } else if (strcasecmp(command, "WRITE") == 0) {
            handle_write(nm_ip, nm_port);
        } else if (strcasecmp(command, "CREATE FILE") == 0) {
            handle_create(nm_ip, nm_port, 0);
        } else if (strcasecmp(command, "CREATE DIR") == 0) {
            handle_create(nm_ip, nm_port, 1);
        } else if (strcasecmp(command, "DELETE FILE") == 0) {
            handle_delete(nm_ip, nm_port, 0);
        } else if (strcasecmp(command, "DELETE DIR") == 0) {
            handle_delete(nm_ip, nm_port, 1);
        } else if (strcasecmp(command, "LIST") == 0) {
            handle_list(nm_ip, nm_port);
        } else if (strcasecmp(command, "GET_INFO") == 0) {
            handle_get_info(nm_ip, nm_port);
        } else if (strcasecmp(command, "STREAM") == 0) {
            handle_stream(nm_ip, nm_port);
        } else {
            printf("Invalid command. Please try again.\n");
        }
    }

    return 0;
}

// Helper function to connect to the Naming Server
int connect_to_naming_server(const char* nm_ip, int nm_port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(nm_port);
    if (inet_pton(AF_INET, nm_ip, &nm_addr.sin_addr) <= 0) {
        perror("Invalid NM IP address");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Connection to Naming Server failed");
        close(sock);
        return -1;
    }

    return sock;
}

// Function to handle READ operation
void handle_read(const char* nm_ip, int nm_port) {
    printf("Enter file path to read: ");
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send READ request to Naming Server
    int32_t opcode = htonl(OP_READ);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive response from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive status from Naming Server");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status != STATUS_SUCCESS) {
        // Receive error message
        int32_t msg_len;
        if (recv(nm_sock, &msg_len, sizeof(msg_len), 0) <= 0) {
            perror("Failed to receive error message length");
            close(nm_sock);
            return;
        }
        msg_len = ntohl(msg_len);
        char* error_msg = malloc(msg_len + 1);
        if (recv(nm_sock, error_msg, msg_len, 0) <= 0) {
            perror("Failed to receive error message");
            free(error_msg);
            close(nm_sock);
            return;
        }
        error_msg[msg_len] = '\0';
        printf("Error: %s\n", error_msg);
        free(error_msg);
        close(nm_sock);
        return;
    }

    // Receive Storage Server info
    int32_t ip_len, port;
    if (recv(nm_sock, &ip_len, sizeof(ip_len), 0) <= 0 ||
        recv(nm_sock, &port, sizeof(port), 0) <= 0) {
        perror("Failed to receive Storage Server info");
        close(nm_sock);
        return;
    }
    ip_len = ntohl(ip_len);
    port = ntohl(port);

    char* ss_ip = malloc(ip_len + 1);
    if (recv(nm_sock, ss_ip, ip_len, 0) <= 0) {
        perror("Failed to receive Storage Server IP");
        free(ss_ip);
        close(nm_sock);
        return;
    }
    ss_ip[ip_len] = '\0';
    close(nm_sock);

    // Connect to Storage Server
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) {
        perror("Socket creation failed");
        free(ss_ip);
        return;
    }

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Invalid Storage Server IP address");
        free(ss_ip);
        close(ss_sock);
        return;
    }

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Connection to Storage Server failed");
        free(ss_ip);
        close(ss_sock);
        return;
    }
    free(ss_ip);

    // Send READ request to Storage Server
    opcode = htonl(OP_READ);
    if (send(ss_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Storage Server");
        close(ss_sock);
        return;
    }

    // Send path
    if (send(ss_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(ss_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Storage Server");
        close(ss_sock);
        return;
    }

    // Receive file size
    int32_t file_size_net;
    if (recv(ss_sock, &file_size_net, sizeof(file_size_net), 0) <= 0) {
        perror("Failed to receive file size");
        close(ss_sock);
        return;
    }
    int32_t file_size = ntohl(file_size_net);

    if (file_size < 0) {
        printf("File not found on Storage Server.\n");
        close(ss_sock);
        return;
    }

    // Receive file content
    printf("File content:\n");
    int bytes_received = 0;
    while (bytes_received < file_size) {
        char buffer[BUFFER_SIZE];
        int chunk = recv(ss_sock, buffer, sizeof(buffer), 0);
        if (chunk <= 0) {
            perror("Failed to receive file content");
            break;
        }
        fwrite(buffer, 1, chunk, stdout);
        bytes_received += chunk;
    }
    printf("\n");

    close(ss_sock);
}

// Function to handle WRITE operation
void handle_write(const char* nm_ip, int nm_port) {
    printf("Enter file path to write: ");
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    printf("Enter data to write (end with an empty line):\n");
    char data[BUFFER_SIZE * 10] = {0};
    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "\n") == 0) break;
        strcat(data, line);
    }

    printf("Choose write mode (1 for synchronous, 2 for asynchronous): ");
    int mode;
    if (scanf("%d", &mode) != 1 || (mode != WRITE_MODE_SYNC && mode != WRITE_MODE_ASYNC)) {
        printf("Invalid write mode selected.\n");
        getchar(); // consume newline or invalid input
        return;
    }
    getchar(); // consume newline

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send WRITE request to Naming Server
    int32_t opcode = htonl(OP_WRITE);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Send write mode
    int32_t write_mode = htonl(mode);
    if (send(nm_sock, &write_mode, sizeof(write_mode), 0) < 0) {
        perror("Failed to send write mode");
        close(nm_sock);
        return;
    }

    // Receive response from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive status from Naming Server");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status != STATUS_SUCCESS) {
        // Receive error message
        int32_t msg_len;
        if (recv(nm_sock, &msg_len, sizeof(msg_len), 0) <= 0) {
            perror("Failed to receive error message length");
            close(nm_sock);
            return;
        }
        msg_len = ntohl(msg_len);
        char* error_msg = malloc(msg_len + 1);
        if (recv(nm_sock, error_msg, msg_len, 0) <= 0) {
            perror("Failed to receive error message");
            free(error_msg);
            close(nm_sock);
            return;
        }
        error_msg[msg_len] = '\0';
        printf("Error: %s\n", error_msg);
        free(error_msg);
        close(nm_sock);
        return;
    }

    // Receive Storage Server info
    int32_t ip_len, port;
    if (recv(nm_sock, &ip_len, sizeof(ip_len), 0) <= 0 ||
        recv(nm_sock, &port, sizeof(port), 0) <= 0) {
        perror("Failed to receive Storage Server info");
        close(nm_sock);
        return;
    }
    ip_len = ntohl(ip_len);
    port = ntohl(port);

    char* ss_ip = malloc(ip_len + 1);
    if (recv(nm_sock, ss_ip, ip_len, 0) <= 0) {
        perror("Failed to receive Storage Server IP");
        free(ss_ip);
        close(nm_sock);
        return;
    }
    ss_ip[ip_len] = '\0';
    close(nm_sock);

    // Connect to Storage Server
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) {
        perror("Socket creation failed");
        free(ss_ip);
        return;
    }

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Invalid Storage Server IP address");
        free(ss_ip);
        close(ss_sock);
        return;
    }

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Connection to Storage Server failed");
        free(ss_ip);
        close(ss_sock);
        return;
    }
    free(ss_ip);

    // Send WRITE request to Storage Server
    // Send opcode
    opcode = htonl(OP_WRITE);
    if (send(ss_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Storage Server");
        close(ss_sock);
        return;
    }

    // Send path
    if (send(ss_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(ss_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Storage Server");
        close(ss_sock);
        return;
    }

    // Send write mode
    if (send(ss_sock, &write_mode, sizeof(write_mode), 0) < 0) {
        perror("Failed to send write mode to Storage Server");
        close(ss_sock);
        return;
    }

    // Send data length
    int32_t data_len = htonl(strlen(data));
    if (send(ss_sock, &data_len, sizeof(data_len), 0) < 0) {
        perror("Failed to send data length to Storage Server");
        close(ss_sock);
        return;
    }

    // Send data
    if (send(ss_sock, data, strlen(data), 0) < 0) {
        perror("Failed to send data to Storage Server");
        close(ss_sock);
        return;
    }

    // For synchronous write, wait for acknowledgment
    if (mode == WRITE_MODE_SYNC) { // Synchronous
        int32_t write_status;
        if (recv(ss_sock, &write_status, sizeof(write_status), 0) <= 0) {
            perror("Failed to receive write status from Storage Server");
            close(ss_sock);
            return;
        }
        write_status = ntohl(write_status);
        if (write_status == STATUS_SUCCESS) {
            printf("Data written successfully (synchronous).\n");
        } else {
            printf("Failed to write data.\n");
        }
    } else { // Asynchronous
        // Receive immediate acknowledgment
        int32_t ack_status;
        if (recv(ss_sock, &ack_status, sizeof(ack_status), 0) <= 0) {
            perror("Failed to receive acknowledgment from Storage Server");
            close(ss_sock);
            return;
        }
        ack_status = ntohl(ack_status);
        if (ack_status == STATUS_SUCCESS) {
            printf("Write request accepted (asynchronous).\n");
            // The Storage Server will later inform the NM about completion
        } else {
            printf("Failed to initiate asynchronous write.\n");
        }
    }

    close(ss_sock);
}


// Function to handle CREATE operation
void handle_create(const char* nm_ip, int nm_port, int is_directory) {
    const char* op_str = is_directory ? "directory" : "file";
    printf("Enter path to create %s: ", op_str);
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send CREATE request to Naming Server
    int32_t opcode = htonl(is_directory ? OP_CREATE_DIR : OP_CREATE_FILE);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive acknowledgment from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive acknowledgment");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status == STATUS_SUCCESS) {
        printf("%s created successfully.\n", is_directory ? "Directory" : "File");
    } else {
        printf("Failed to create %s.\n", is_directory ? "directory" : "file");
    }

    close(nm_sock);
}

// Function to handle DELETE operation
void handle_delete(const char* nm_ip, int nm_port, int is_directory) {
    const char* op_str = is_directory ? "directory" : "file";
    printf("Enter path to delete %s: ", op_str);
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send DELETE request to Naming Server
    int32_t opcode = htonl(is_directory ? OP_DELETE_DIR : OP_DELETE_FILE);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive acknowledgment from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive acknowledgment");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status == STATUS_SUCCESS) {
        printf("%s deleted successfully.\n", is_directory ? "Directory" : "File");
    } else {
        printf("Failed to delete %s.\n", is_directory ? "directory" : "file");
    }

    close(nm_sock);
}

// Function to handle LIST operation
void handle_list(const char* nm_ip, int nm_port) {
    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send LIST request to Naming Server
    int32_t opcode = htonl(OP_LIST);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive acknowledgment from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive acknowledgment");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status != STATUS_SUCCESS) {
        printf("Failed to list accessible paths.\n");
        close(nm_sock);
        return;
    }

    // Receive number of paths
    int32_t num_paths;
    if (recv(nm_sock, &num_paths, sizeof(num_paths), 0) <= 0) {
        perror("Failed to receive number of paths");
        close(nm_sock);
        return;
    }
    num_paths = ntohl(num_paths);

    printf("Accessible paths:\n");
    for (int i = 0; i < num_paths; ++i) {
        int32_t path_len;
        if (recv(nm_sock, &path_len, sizeof(path_len), 0) <= 0) {
            perror("Failed to receive path length");
            close(nm_sock);
            return;
        }
        path_len = ntohl(path_len);

        char* path = malloc(path_len + 1);
        if (recv(nm_sock, path, path_len, 0) <= 0) {
            perror("Failed to receive path");
            free(path);
            close(nm_sock);
            return;
        }
        path[path_len] = '\0';
        printf("%s\n", path);
        free(path);
    }

    close(nm_sock);
}

// Function to handle GET_INFO operation
void handle_get_info(const char* nm_ip, int nm_port) {
    printf("Enter file path to get info: ");
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send GET_INFO request to Naming Server
    int32_t opcode = htonl(OP_GET_INFO);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive response from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive status from Naming Server");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status != STATUS_SUCCESS) {
        // Receive error message
        int32_t msg_len;
        if (recv(nm_sock, &msg_len, sizeof(msg_len), 0) <= 0) {
            perror("Failed to receive error message length");
            close(nm_sock);
            return;
        }
        msg_len = ntohl(msg_len);
        char* error_msg = malloc(msg_len + 1);
        if (recv(nm_sock, error_msg, msg_len, 0) <= 0) {
            perror("Failed to receive error message");
            free(error_msg);
            close(nm_sock);
            return;
        }
        error_msg[msg_len] = '\0';
        printf("Error: %s\n", error_msg);
        free(error_msg);
        close(nm_sock);
        return;
    }

    // Receive file size and permissions
    int32_t file_size_net;
    int32_t perm_len_net;
    if (recv(nm_sock, &file_size_net, sizeof(file_size_net), 0) <= 0 ||
        recv(nm_sock, &perm_len_net, sizeof(perm_len_net), 0) <= 0) {
        perror("Failed to receive file info");
        close(nm_sock);
        return;
    }
    int32_t file_size = ntohl(file_size_net);
    int32_t perm_len = ntohl(perm_len_net);

    char* permissions = malloc(perm_len + 1);
    if (recv(nm_sock, permissions, perm_len, 0) <= 0) {
        perror("Failed to receive permissions");
        free(permissions);
        close(nm_sock);
        return;
    }
    permissions[perm_len] = '\0';

    printf("File size: %d bytes\n", file_size);
    printf("Permissions: %s\n", permissions);

    free(permissions);
    close(nm_sock);
}

// Function to handle STREAM operation
void handle_stream(const char* nm_ip, int nm_port) {
    printf("Enter audio file path to stream: ");
    char path[BUFFER_SIZE];
    if (fgets(path, sizeof(path), stdin) == NULL) {
        printf("Failed to read input.\n");
        return;
    }
    path[strcspn(path, "\n")] = '\0';

    int nm_sock = connect_to_naming_server(nm_ip, nm_port);
    if (nm_sock < 0) return;

    // Send STREAM request to Naming Server
    int32_t opcode = htonl(OP_STREAM);
    if (send(nm_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Naming Server");
        close(nm_sock);
        return;
    }

    // Send path
    int32_t path_len = htonl(strlen(path));
    if (send(nm_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(nm_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Naming Server");
        close(nm_sock);
        return;
    }

    // Receive response from Naming Server
    int32_t status;
    if (recv(nm_sock, &status, sizeof(status), 0) <= 0) {
        perror("Failed to receive status from Naming Server");
        close(nm_sock);
        return;
    }
    status = ntohl(status);

    if (status != STATUS_SUCCESS) {
        // Receive error message
        int32_t msg_len;
        if (recv(nm_sock, &msg_len, sizeof(msg_len), 0) <= 0) {
            perror("Failed to receive error message length");
            close(nm_sock);
            return;
        }
        msg_len = ntohl(msg_len);
        char* error_msg = malloc(msg_len + 1);
        if (recv(nm_sock, error_msg, msg_len, 0) <= 0) {
            perror("Failed to receive error message");
            free(error_msg);
            close(nm_sock);
            return;
        }
        error_msg[msg_len] = '\0';
        printf("Error: %s\n", error_msg);
        free(error_msg);
        close(nm_sock);
        return;
    }

    // Receive Storage Server info
    int32_t ip_len, port;
    if (recv(nm_sock, &ip_len, sizeof(ip_len), 0) <= 0 ||
        recv(nm_sock, &port, sizeof(port), 0) <= 0) {
        perror("Failed to receive Storage Server info");
        close(nm_sock);
        return;
    }
    ip_len = ntohl(ip_len);
    port = ntohl(port);

    char* ss_ip = malloc(ip_len + 1);
    if (recv(nm_sock, ss_ip, ip_len, 0) <= 0) {
        perror("Failed to receive Storage Server IP");
        free(ss_ip);
        close(nm_sock);
        return;
    }
    ss_ip[ip_len] = '\0';
    close(nm_sock);

    // Connect to Storage Server
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) {
        perror("Socket creation failed");
        free(ss_ip);
        return;
    }

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0) {
        perror("Invalid Storage Server IP address");
        free(ss_ip);
        close(ss_sock);
        return;
    }

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Connection to Storage Server failed");
        free(ss_ip);
        close(ss_sock);
        return;
    }
    free(ss_ip);

    // Send STREAM request to Storage Server
    // Send opcode
    opcode = htonl(OP_STREAM);
    if (send(ss_sock, &opcode, sizeof(opcode), 0) < 0) {
        perror("Failed to send opcode to Storage Server");
        close(ss_sock);
        return;
    }

    // Send path
    if (send(ss_sock, &path_len, sizeof(path_len), 0) < 0 ||
        send(ss_sock, path, strlen(path), 0) < 0) {
        perror("Failed to send path to Storage Server");
        close(ss_sock);
        return;
    }

    // Prepare to receive audio data and stream it to media player
    printf("Streaming audio...\n");

    // Use popen to open a pipe to the media player
    FILE* player = popen("mpv --no-video -", "w");
    if (!player) {
        perror("Failed to open media player");
        close(ss_sock);
        return;
    }

    // Receive data from SS and write to player
    char buffer[BUFFER_SIZE];
    int bytes_received;
    while ((bytes_received = recv(ss_sock, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, player);
        fflush(player);
    }

    if (bytes_received < 0) {
        perror("Error while receiving audio data");
    }

    pclose(player);
    close(ss_sock);
}

