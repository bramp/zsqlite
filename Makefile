OBJS = snappy-sqlite.o
CC = clang++
DEBUG = -g
CFLAGS = -Wall -c $(DEBUG)
LFLAGS = -Wall -Wl,--no-as-needed -lsnappy $(DEBUG)

snappy-sqlite : $(OBJS)
	$(CC) $(LFLAGS) $(OBJS) -o $@

snappy-sqlite.o : snappy-sqlite.cc
	$(CC) $(CFLAGS) snappy-sqlite.cc

clean:
	rm *.o snappy-sqlite

.PHONY: clean
