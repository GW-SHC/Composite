#!/bin/sh

cp rumpcos.o llboot.o
./cos_linker "llboot.o, :" ./gen_client_stub -v
