LIB=lib/libfibtask.a
all: $(LIB) simplehttp

AS=gcc -c
CC=gcc
CFLAGS=-Wall -c -Iinclude -g

context.o: src/context.S
	$(AS) src/context.S

task.o: src/task.c include/task.h include/chain.h include/sysdef.h include/spinlock.h include/timestamp.h
	$(CC) $(CFLAGS) -O3 src/task.c

epoll.o: src/epoll.c include/task.h include/chain.h include/sysdef.h include/spinlock.h include/timestamp.h
	$(CC) $(CFLAGS) -O3 src/epoll.c

simplehttp.o: simplehttp.c include/task.h include/chain.h include/sysdef.h include/spinlock.h include/timestamp.h
	$(CC) $(CFLAGS) -O2 simplehttp.c

$(LIB): context.o task.o epoll.o
	ar rvc $(LIB) context.o task.o epoll.o

simplehttp: simplehttp.o $(LIB)
	$(CC) -o simplehttp simplehttp.o $(LIB)

clean:
	rm -f *.o simplehttp $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp include/task.h /usr/local/include
