# setup globals for both compilation tasks
EXTENSION       = pg_ctblmgr
EXTVERSION      = $(shell grep default_version $(EXTENSION).control | \
                  sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/" )
DOCS			= README.md
PG_CONFIG      ?= pg_config
PGINCLUDEDIR    = $(shell $(PG_CONFIG) --includedir-server)
PGLIBDIR        = $(shell $(PG_CONFIG) --libdir)
PGXS            = $(shell $(PG_CONFIG) --pgxs)
DEBUG			= -g -DDEBUG
CC              = gcc
EXTRA_CLEAN     = src/*.o src/*.so *.so *.o sql/$(EXTENSION)--$(EXTVERSION).sql

PG_CFLAGS		= $(DEBUG)
SRCS			= $(wildcard src/*.c)
OBJS			= $(SRCS:.c=.o)
MODULE_big		= pg_ctblmgr_decoder

include $(PGXS)
