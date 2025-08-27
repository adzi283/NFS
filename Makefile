# Makefile

CC = gcc
CFLAGS = -Wall -pthread
DEPS = common.h protocol.h

all: client naming_server storage_server

client: client.o
	$(CC) $(CFLAGS) -o client client.o

naming_server: naming_server.o
	$(CC) $(CFLAGS) -o naming_server naming_server.o

storage_server: storage_server.o
	$(CC) $(CFLAGS) -o storage_server storage_server.o

client.o: client.c $(DEPS)
	$(CC) $(CFLAGS) -c client.c

naming_server.o: naming_server.c $(DEPS)
	$(CC) $(CFLAGS) -c naming_server.c

storage_server.o: storage_server.c $(DEPS)
	$(CC) $(CFLAGS) -c storage_server.c

clean:
	rm -f *.o client naming_server storage_server
