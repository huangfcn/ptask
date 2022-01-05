LIB=lib/libfiber.a
all: $(LIB) simplehttp generator reader_writer blockq rwlock fiberbq pipe_ring

AS=gcc
CC=gcc
CXX=g++
CFLAGS= -Iinclude -I. -Wall -march=native -D__1_N_MODEL__

INCFILES=include/task.h      \
         include/chain.h     \
         include/sysdef.h    \
         include/spinlock.h  \
         include/timestamp.h \
         include/epoll.h

.objs/context.o: src/context.S
	$(AS) -c $< -o $@

.objs/%.o: src/%.c $(INCFILES)
	$(CC) $(CFLAGS) -g -O3 -c $< -o $@

.objs/%.o: %.c $(INCFILES)
	$(CC) $(CFLAGS) -g -O3 -c $< -o $@

.objs/%.o: src/%.cpp $(INCFILES)
	$(CXX) $(CFLAGS) -std=c++14 -g -O3 -c $< -o $@

$(LIB): .objs/context.o .objs/task.o .objs/epoll.o
	ar rvc $(LIB) .objs/context.o .objs/task.o .objs/epoll.o

simplehttp: .objs/simplehttp.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

generator: .objs/generator.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

reader_writer: .objs/reader_writer.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

blockq: .objs/blockq.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

rwlock: .objs/rwlock.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

fiberbq: .objs/fiberbq.o $(LIB)
	$(CC) -o $@ $< $(LIB) -lpthread

pipe_ring: .objs/pipe.o .objs/pipe_ring.o $(LIB)
	$(CC) -o $@ .objs/pipe.o .objs/pipe_ring.o $(LIB) -lpthread

clean:
	rm -f .objs/*.o simplehttp generator reader_writer blockq rwlock fiberbq pipe_ring $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp $(INCFILES) /usr/local/include/fiber
