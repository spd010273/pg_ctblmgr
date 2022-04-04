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
T_CS_SRCS		= ../src/lib/barrier.c ../src/lib/shm.c ../src/lib/slab.c ../src/lib/xlog.c ../src/lib/changeset.c ../src/lib/util.c ../src/lib/jsmn.c src/t_changeset.c
T_CS_OBJS		= $(T_CS_SRCS:.c=.o)
T_B_SRCS		= ../src/lib/barrier.c ../src/lib/shm.c ../src/lib/slab.c ../src/lib/util.c ../src/lib/buffer.c ../src/lib/trie.c ../src/lib/slpq.c src/t_buffer.c
T_B_OBJS		= $(T_B_SRCS:.c=.o)
T_Q_SRCS		= ../src/lib/barrier.c ../src/lib/query.c ../src/lib/util.c src/t_query.c
T_Q_OBJS		= $(T_Q_SRCS:.c=.o)
T_SHM_SRCS		= ../src/lib/shm.c ../src/lib/barrier.c src/t_shm.c
T_SHM_OBJS		= $(T_SHM_SRCS:.c=.o)
T_SLAB_SRCS		= ../src/lib/barrier.c ../src/lib/shm.c ../src/lib/slab.c src/t_slab.c
T_SLAB_OBJS		= $(T_SLAB_SRCS:.c=.o)
T_T_SRCS 		= ../src/lib/barrier.c ../src/lib/shm.c ../src/lib/slab.c ../src/lib/trie.c src/t_trie.c
T_T_OBJS		= $(T_T_SRCS:.c=.o)
LDFLAGS			= -lm -lpq -lrt

all: t_buffer t_changeset t_query t_shm t_slab t_trie

t_changeset: $(T_CS_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

t_buffer: $(T_B_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

t_query: $(T_Q_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

t_shm: $(T_SHM_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

t_slab: $(T_SLAB_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

t_trie: $(T_T_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(T_CS_OBJS)
	rm -f $(T_B_OBJS)
	rm -f $(T_Q_OBJS)
	rm -f $(T_SHM_OBJS)
	rm -f $(T_SLAB_OBJS)
	rm -f $(T_T_OBJS)
	rm -f t_*
