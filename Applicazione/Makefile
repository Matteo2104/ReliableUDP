CC = gcc
CFLAGS = -DP=$(P_VALUE) -DTIMEOUT=$(TIMEOUT_VALUE) -DWAIT=$(WAIT_VALUE)
AT_FLAG = 

P_VALUE ?= 0
TIMEOUT_VALUE ?= 100000
WAIT_VALUE ?= 15

ifdef AT
	ifeq ($(filter $(AT),0 1),)
      $(error Il valore di AT deve essere 0 o 1)
  endif
  
  ifeq ($(filter $(AT),0 1),0)
  	AT_FLAG := 
  endif
  AT_FLAG := -DAT
endif

all: compile_objects compile_clientservers clean

compile_objects: pseudoTCP.o dynamiclist.o

compile_clientservers: client.o server.o
	$(CC) $(CFLAGS) -o client client.o pseudoTCP.o dynamiclist.o 
	$(CC) $(CFLAGS) -o server server.o pseudoTCP.o dynamiclist.o 

pseudoTCP.o: lib/pseudoTCP.c
	$(CC) $(CFLAGS) $(AT_FLAG) -c -o pseudoTCP.o lib/pseudoTCP.c

dynamiclist.o: lib/dynamiclist.c
	$(CC) $(CFLAGS) -c -o dynamiclist.o lib/dynamiclist.c

client.o: client.c
	$(CC) $(CFLAGS) -c -o client.o client.c

server.o: server.c
	$(CC) $(CFLAGS) -c -o server.o server.c
	
clean:
	rm -f client.o server.o pseudoTCP.o dynamiclist.o
