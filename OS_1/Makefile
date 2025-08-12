CC = gcc
CFLAGS = -lrt -lpthread

all: sharedmem child parent

sharedmem: sharedmem.c
	$(CC) sharedmem.c -o sharedmem $(CFLAGS)

child: child.c
	$(CC) child.c -o child $(CFLAGS)

parent: parent.c
	$(CC) parent.c -o parent $(CFLAGS)

clean:
	rm -f sharedmem child parent
