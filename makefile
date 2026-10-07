# Makefile for the hero.gb GBDK-2020 project.
# Requires the GBDK-2020 toolchain with lcc on your PATH.
# https://github.com/gbdk-2020/gbdk-2020

CC  := lcc
ROM := hero.gb
SRC := main.c

all: $(ROM)

$(ROM): $(SRC)
	$(CC) -o $(ROM) $(SRC)

clean:
	rm -f $(ROM) *.o *.lst *.map *.sym *.noi *.ihx *.adb

.PHONY: all clean
