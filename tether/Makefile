# Tether - Game Boy build (GBDK-2020)
# make            build tether.gb
# make gfx        regenerate src/gfx.c / src/gfx.h from assets and tools/make_gfx.py
# Uses lcc from your PATH, or set GBDK to the GBDK-2020 folder: make GBDK=~/gbdk
LCC  := $(if $(GBDK),$(GBDK)/bin/lcc,lcc)
CFLAGS := -Wl-m -Wm-yn"TETHER" -Wf--opt-code-speed

SRC := src/main.c src/hose.c src/levels.c src/gfx.c

tether.gb: $(SRC) src/game.h src/gfx.h
	$(LCC) $(CFLAGS) -o $@ $(SRC)

gfx: assets/hero.png tools/make_gfx.py
	python3 tools/make_gfx.py assets/hero.png src/gfx.c src/gfx.h

clean:
	rm -f tether.gb *.map *.noi *.ihx *.lst *.o *.sym src/*.o src/*.lst src/*.asm src/*.sym

.PHONY: gfx clean
