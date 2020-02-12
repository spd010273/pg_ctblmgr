DEBUG			= -g -DDEBUG
PG_CONFIG  	   ?= pg_config
PG_INCLUDEDIR	= $(shell $(PG_CONFIG) --includedir)
PG_LIBDIR		= $(shell $(PG_CONFIG) --libdir)
CC				= gcc
INCLUDE			= -I. -I./../src/lib/ -I./src -I$(PG_INCLUDEDIR)
LIBS			= -L$(PG_LIBDIR) 
FLAGS			= $(DEBUG) -O2 -pipe
STRICTURES		= -Wall -Wp,-D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector-strong \
				  --param=ssp-buffer-size=4 -grecord-gcc-switches \
				  -Wmissing-prototypes -Wpointer-arith -Wdeclaration-after-statement \
				  -Wendif-labels -Wmissing-format-attribute -Wformat-security \
				  -fno-strict-aliasing -fwrapv -fexcess-precision=standard
ARCHFLAGS		= -m64 -mtune=generic
CFLAGS			= $(INCLUDE) $(LIBS) $(FLAGS) $(STRICTURES) $(ARCHFLAGS)
T_CS_SRCS		= ../src/lib/changeset.c ../src/lib/util.c ../src/lib/jsmn.c src/t_changeset.c
T_CS_OBJS		= $(T_CS_SRCS:.c=.o)
LDFLAGS			= -lm -lpq

t_changeset: $(T_CS_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(T_CS_OBJS)
	rm -f t_changeset
