# Master makefile for pg_ctblmgr, this builds:
# Logical Decoder Plugin - located in server/ (for server-side code)
# Replication Client Service - locates in service/ (for service related code)
SERVICE_SOURCES = $(wildcard service/test/src/t_*.c)
SERVICE_TESTS	= $(SERVICE_SOURCES:.c=)

all: pg_ctblmgr_decoder pg_ctblmgr_service

pg_ctblmgr_service:
	$(MAKE) -C service
	cp service/pg_ctblmgr ./

pg_ctblmgr_decoder:
	$(MAKE) -C server

install: all
	$(MAKE) -C server install

make_test:
	$(MAKE) -C service/test

check: make_test
	test/run_tests.pl

.PHONY: clean

clean:
	$(MAKE) -C service clean
	$(MAKE) -C server clean
	rm -f pg_ctblmgr
