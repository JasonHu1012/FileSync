INCLUDE = ../Clibrary/include/
LIB = ../Clibrary/lib/

CC = gcc
CFLAGS = -Wall -I$(INCLUDE)

.PHONY: clean

all: server client

server: server.c utils.o $(LIB)libjson.a $(LIB)libarg_parser.a
	$(CC) -o $@ $(CFLAGS) $^

client: client.c utils.o $(LIB)libjson.a $(LIB)libarg_parser.a
	$(CC) -o $@ $(CFLAGS) $^

utils.o: utils.c utils.h
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm -rf server client utils.o
