LIB=lib/libfibtask.a
all: $(LIB) simplehttp

AS=gcc -c
CC=gcc
CFLAGS=-Wall -c -Iinclude -O3

context.o: src/context.S
	$(AS) src/context.S

task.o: src/task.c include/task.h include/chain.h include/sysdef.h include/spinlock.h include/timestamp.h
	$(CC) $(CFLAGS) src/task.c

$(LIB): context.o task.o
	ar rvc $(LIB) context.o task.o

simplehttp: simplehttp.o $(LIB)
	$(CC) -o simplehttp simplehttp.o $(LIB)

clean:
	rm -f *.o simplehttp $(LIB)

install: $(LIB)
	cp $(LIB) /usr/local/lib
	cp include/task.h /usr/local/include