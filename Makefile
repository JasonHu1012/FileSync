INCLUDE_CLIB = ../Clibrary/include/
INCLUDE_LOCAL = include/
SRC = src/
OBJ = obj/
LIB = ../Clibrary/lib/

CC = gcc
CFLAGS = -Wall -I$(INCLUDE_LOCAL) -I$(INCLUDE_CLIB)

.PHONY: clean

all: server client

server: $(SRC)server.c $(OBJ)server_config.o $(OBJ)utils.o $(LIB)libjson.a $(LIB)libarg_parser.a
	$(CC) -o $@ $(CFLAGS) $^

client: $(SRC)client.c $(OBJ)client_config.o $(OBJ)utils.o $(LIB)libjson.a $(LIB)libarg_parser.a
	$(CC) -o $@ $(CFLAGS) $^

$(OBJ)%.o: $(SRC)%.c $(INCLUDE_LOCAL)%.h | $(OBJ)
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm -rf server client $(OBJ)

$(OBJ):
	mkdir -p $(OBJ)
