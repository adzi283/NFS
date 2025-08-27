// protocol.h

#ifndef PROTOCOL_H
#define PROTOCOL_H

// Operation codes
#define OP_READ             1
#define OP_WRITE            2
#define OP_CREATE_FILE      3
#define OP_CREATE_DIR       4
#define OP_DELETE_FILE      5
#define OP_DELETE_DIR       6
#define OP_LIST             7
#define OP_GET_INFO         8
#define OP_STREAM           9

// Status codes
#define STATUS_SUCCESS            0
#define STATUS_FAILURE            1
#define STATUS_FILE_NOT_FOUND     2
#define STATUS_ACCESS_DENIED      3
#define STATUS_INVALID_OPERATION  4
#define STATUS_BUSY               5
#define STATUS_ASYNC_COMPLETED    6
#define STATUS_ASYNC_FAILED       7

// Write modes
#define WRITE_MODE_SYNC   1
#define WRITE_MODE_ASYNC  2
#define OP_HEARTBEAT           10
#define OP_ASYNC_WRITE_COMPLETE 11
#endif // PROTOCOL_H
