CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -libverbs -lpthread

# Source files
COMMON_SRC = common.c
SERVER_SRC = rdma_server.c
CLIENT_SRC = rdma_client.c

# Object files
COMMON_OBJ = $(COMMON_SRC:.c=.o)
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Executables
SERVER = rdma_server
CLIENT = rdma_client

# Default target
all: $(SERVER) $(CLIENT)

# Server compilation
$(SERVER): $(SERVER_OBJ) $(COMMON_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# Client compilation
$(CLIENT): $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

# Generic rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(SERVER) $(CLIENT) *.o

.PHONY: all clean