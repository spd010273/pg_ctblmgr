EXTENSION       = pg_ctblmgr
EXTVERSION      = $(shell grep default_version $(EXTENSION).control | \
                  sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/" )
DOCS			= README.md
PG_CONFIG      ?= pg_config
PGLIBDIR        = $(shell $(PG_CONFIG) --libdir)
PGINCLUDEDIR    = $(shell $(PG_CONFIG) --includedir)
DEBUG			= -g -DDEBUG

CC              = gcc
PG_LIBS         = -lm -L$(PGLIBDIR) -lpq
PG_CFLAGS       = -I$(PGINCLUDEDIR) -Isrc/lib/ -Isrc/ $(LIBS) $(DEBUG)
SRCS            = $(wildcard src/lib/*.c) $(wildcard src/*.c)
OBJS            = $(SRCS:.c=.o)
PGXS            = $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS     = $(PG_CFLAGS)
EXTRA_CLEAN     = src/*.o src/*.so *.so *.o sql/$(EXTENSION)--$(EXTVERSION).sql
PROGRAM			= pg_ctblmgr
MODULES			= pg_ctblmgr_decoder
#all: sql/$(EXTENSION)--$(EXTVERSION).sql

#sql/$(EXTENSION)--$(EXTVERSION).sql: $(sort $(wildcard sql/tables/*.sql)) $(sort $(wildcard sql/functions/*.sql))
#	cat $^ > $@

#pg_ctblmgr: $(OBJS)
#	$(CC) -o pg_ctblmgr $(OBJS) $(PC_CFLAGS) -lpq -lm

include $(PGXS)
