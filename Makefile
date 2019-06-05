# Master makefile for pg_ctblmgr, this builds:
# Logical Decoder Plugin - located in server/ (for server-side code)
# Replication Client Service - locates in service/ (for service related code)

all: pg_ctblmgr_decoder pg_ctblmgr_service

pg_ctblmgr_service:
	$(MAKE) -C service
	cp service/pg_ctblmgr ./

pg_ctblmgr_decoder:
	$(MAKE) -C server

install: all
	$(MAKE) -C server install

.PHONY: clean

clean:
	$(MAKE) -C service clean
	$(MAKE) -C server clean
	rm pg_ctblmgr
