OBJS = snappy-sqlite.o
CC = clang++
DEBUG = -g
CFLAGS = -Wall -c $(DEBUG)
LFLAGS = -Wall -Wl,--no-as-needed -lsnappy -llzo2 $(DEBUG)

snappy-sqlite : $(OBJS)
	$(CC) $(LFLAGS) $(OBJS) -o $@

snappy-sqlite.o : snappy-sqlite.cc
	$(CC) $(CFLAGS) snappy-sqlite.cc

test: snappy-sqlite
	./snappy-sqlite /home/bramp/personal/map/acs/acs2010_5yr/master.sqlite test.sqlite.sz
	./snappy-sqlite /home/bramp/personal/map/acs/acs2010_5yr/05000.sqlite 05000.sqlite.sz

test2: snappy-sqlite
	./snappy-sqlite blah blah

clean:
	rm *.o snappy-sqlite

.PHONY: clean test test2
