LIB=lib/libfiber.a
all: $(LIB) simplehttp generator

AS=gcc -c
CC=gcc
CFLAGS=-Wall -c -Iinclude -g

INCFILES=include/task.h      \
         include/chain.h     \
         include/sysdef.h    \
         include/spinlock.h  \
         include/timestamp.h \
         include/epoll.h

context.o: src/context.S
	$(AS) src/context.S

task.o: src/task.c $(INCFILES)
	$(CC) $(CFLAGS) -O3 src/task.c

epoll.o: src/epoll.c $(INCFILES)
	$(CC) $(CFLAGS) -O3 src/epoll.c

simplehttp.o: simplehttp.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 simplehttp.c

generator.o: generator.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 generator.c

$(LIB): context.o task.o epoll.o
	ar rvc $(LIB) context.o task.o epoll.o

simplehttp: simplehttp.o $(LIB)
	$(CC) -o simplehttp simplehttp.o $(LIB) -lpthread

generator: generator.o $(LIB)
	$(CC) -o generator generator.o $(LIB) -lpthread

clean:
	rm -f *.o simplehttp generator $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp $(INCFILES) /usr/local/include/fiber
