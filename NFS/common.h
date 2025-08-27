// common.h

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define BUFFER_SIZE 1024

// Shared function prototypes
int connect_to_naming_server(const char* nm_ip, int nm_port);

#endif // COMMON_H
