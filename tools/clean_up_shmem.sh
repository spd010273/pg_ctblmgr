#!/usr/bin/bash
keys=$(ipcs -m | grep -v -E 'postgres' | grep '0x' | awk '{print $1}')

for key in $keys;
do
    ipcrm --shmem-key $key
done

keys=$(ipcs -s | grep -v -E 'postgres' | grep '0x' | awk '{print $1}')

for key in $keys;
do
    ipcrm --semaphore-key $key
done
