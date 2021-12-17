LIB=lib/libfiber.a
all: $(LIB) simplehttp generator reader_writer blockq rwlock fiberbq pipe_ring

AS=gcc -c
CC=gcc
CXX=g++
CFLAGS=-Wall -c -Iinclude -I. -g -D__SCHEDULER_USING_BLOCKQ__

INCFILES=include/task.h      \
         include/chain.h     \
         include/sysdef.h    \
         include/spinlock.h  \
         include/timestamp.h \
         include/epoll.h

context.o: src/context.S
	$(AS) src/context.S

task.o: src/task.c $(INCFILES)
	$(CC) $(CFLAGS) -O3 -march=native src/task.c

epoll.o: src/epoll.c $(INCFILES)
	$(CC) $(CFLAGS) -O3 -march=native src/epoll.c

simplehttp.o: simplehttp.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 simplehttp.c

reader_writer.o: reader_writer.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 reader_writer.c

generator.o: generator.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 generator.c

blockq.o: blockq.c $(INCFILES)
	$(CC) $(CFLAGS) -O3 blockq.c

rwlock.o: rwlock.c $(INCFILES)
	$(CC) $(CFLAGS) -O2 rwlock.c

fiberbq.o: fiberbq.c $(INCFILES) include/fiberq.h
	$(CC) $(CFLAGS) -O2 fiberbq.c

pipe.o: pipe.c $(INCFILES) pipe.h
	$(CC) $(CFLAGS) -march=native -O3 pipe.c

pipe_ring.o: pipe_ring.c $(INCFILES) pipe.h
	$(CC) $(CFLAGS) -O3 pipe_ring.c

$(LIB): context.o task.o epoll.o
	ar rvc $(LIB) context.o task.o epoll.o

simplehttp: simplehttp.o $(LIB)
	$(CC) -o simplehttp simplehttp.o $(LIB) -lpthread

generator: generator.o $(LIB)
	$(CC) -o generator generator.o $(LIB) -lpthread

reader_writer: reader_writer.o $(LIB)
	$(CC) -o reader_writer reader_writer.o $(LIB) -lpthread

blockq: blockq.o $(LIB)
	$(CC) -o blockq blockq.o $(LIB) -lpthread

rwlock: rwlock.o $(LIB)
	$(CC) -o rwlock rwlock.o $(LIB) -lpthread

fiberbq: fiberbq.o $(LIB)
	$(CC) -o fiberbq fiberbq.o $(LIB) -lpthread

pipe_ring: pipe.o pipe_ring.o $(LIB)
	$(CC) -o pipe_ring pipe_ring.o pipe.o $(LIB) -lpthread

clean:
	rm -f *.o simplehttp generator reader_writer blockq rwlock fiberbq pipe_ring $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp $(INCFILES) /usr/local/include/fiber
