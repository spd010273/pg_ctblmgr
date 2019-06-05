DEBUG		= -g -DDEBUG
CC			= gcc
PG_CONFIG  ?= pg_config
CFLAGS 		= -I. -I./lib/ -I$(shell $(PG_CONFIG) --includedir)\
		  	  -L$(shell $(PG_CONFIG) --libdir) $(DEBUG)
SRCS		= $(wildcard src/*.c) $(wildcard src/lib/*.c)
OBJS		= $(SRCS:.c=.o)
LDFLAGS 	= -lm -lpq

pg_ctblmgr: $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(OBJS) pg_ctblmgr
