EXTENSION       = pg_ctblmgr
EXTVERSION      = $(shell grep default_version $(EXTENSION).control | \
                  sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/" )
PG_CONFIG      ?= pg_config
PGLIBDIR        = $(shell $(PG_CONFIG) --libdir)
PGINCLUDEDIR    = $(shell $(PG_CONFIG) --includedir-server)
CC              = gcc
LIBS            = -lm
PG_CFLAGS       = -I$(PGINCLUDEDIR) -Isrc/lib/
SRCS            = $(wildcard src/lib/*.c) $(wildcard src/*.c)
OBJS            = $(SRCS:.c=.o)
PGXS            = $(shell $(PG_CONFIG) --pgxs)
EXTRA_CLEAN     = src/*.o src/*.so *.so *.o sql/$(EXTENSION)--$(EXTVERSION).sql

all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: $(sort $(wildcard sql/tables/*.sql)) $(sort $(wildcard sql/functions/*.sql))
    cat $^ > $@

PG_CPPFLAGS     = -DDEBUG -g $(PG_CFLAGS)

include $(PGXS)
