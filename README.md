# My Network File System

A minimal distributed file system split into three processes:
- **Naming Server (NM):** directory / path mapper with an LRU cache; hands clients the IP:port of the Storage Server (SS) that serves a path.
- **Storage Server (SS):** exposes one or more exported path prefixes, stores files on local disk, and registers with the NM.
- **Client (CLI):** interactive shell that issues file ops via the NM and then talks to the SS for data transfer.

## Build
```bash
make            # builds: naming_server, storage_server, client
```
If `make` fails on your system, compile manually (POSIX, `-pthread`).

## Run (example)
**1) Start the Naming Server**
```bash
./naming_server <BindIP> <ClientPort> <StorageServerPort>
# e.g.
./naming_server 127.0.0.1 5000 5001
```

**2) Start a Storage Server**
```bash
./storage_server <NM_IP> <NM_Port> <SS_NM_Port> <SS_Client_Port> <Paths...> <StorageDir>
# e.g. exports "/" and keeps files under ./storage
mkdir -p storage
./storage_server 127.0.0.1 5001 6001 6002 / ./storage
```

**3) Start the Client**
```bash
./client <NM_IP> <NM_ClientPort>
# e.g.
./client 127.0.0.1 5000
```

## Client commands
At the prompt, type one of:
```
READ
WRITE
CREATE FILE
CREATE DIR
DELETE FILE
DELETE DIR
LIST
GET_INFO
STREAM
```
- **WRITE** asks for synchronous (`1`) or asynchronous (`2`) mode.
- **LIST** prints paths currently known to the NM.
- **STREAM** reads a file in chunks (useful for audio).

## Protocol (summary)
Operation codes used across NM/SS/Client:
```
READ=1, WRITE=2, CREATE_FILE=3, CREATE_DIR=4,
DELETE_FILE=5, DELETE_DIR=6, LIST=7, GET_INFO=8, STREAM=9,
HEARTBEAT=10, ASYNC_WRITE_COMPLETE=11
```
Status codes: `SUCCESS=0`, `FAILURE=1`, `FILE_NOT_FOUND=2`, `ACCESS_DENIED=3`, `INVALID_OPERATION=4`, `BUSY=5`, `ASYNC_COMPLETED=6`, `ASYNC_FAILED=7`.

## Design notes
- **LRU path cache** in the NM speeds up path→server lookups (cache size 20).
- NM accepts both **clients** and **storage servers**; on create/delete it forwards the request to one SS and updates its path map.
- SS persists files inside `<StorageDir>`; it will create parent directories as needed.
- SS supports **sync** and **async** writes; on async completion it notifies the NM.
- All networking is TCP (IPv4).

## Repo layout
```
Makefile
naming_server.c     # NM: accept clients & SS, path map + LRU
storage_server.c    # SS: register with NM; data plane for files
client.c            # CLI for testing the DFS
protocol.h          # shared op/status codes
common.h            # shared helpers, BUFFER_SIZE
```

## Known limitations
- No replication or failover; a path is served by one SS.
- Minimal error handling and no authentication/ACLs.
- File I/O is single-node (one SS); LIST is based on NM's path map.
- Paths are treated as simple strings (no normalization/security checks).

## License
Use freely for coursework and demos. Attribution appreciated.
