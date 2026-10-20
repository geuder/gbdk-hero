# Stealth prototype - Game Boy build (GBDK-2020)
# make            build stealth.gb
# make gfx        regenerate src/gfx.c / src/gfx.h (needs Python 3 + Pillow)
# Uses lcc from your PATH, or set GBDK to the GBDK-2020 folder: make GBDK=~/gbdk
LCC  := $(if $(GBDK),$(GBDK)/bin/lcc,lcc)
CFLAGS := -Wl-m -Wl-j -Wm-yn"STEALTH" -Wf--opt-code-speed

SRC := src/main.c src/cone.c src/vram.c src/gfx.c src/levels.c

stealth.gb: $(SRC) src/game.h src/gfx.h
	$(LCC) $(CFLAGS) -o $@ $(SRC)

gfx: assets/hero.png tools/make_gfx.py
	python3 tools/make_gfx.py assets/hero.png src/gfx.c src/gfx.h

clean:
	rm -f stealth.gb *.map *.noi *.ihx *.lst *.o *.sym src/*.o src/*.lst src/*.asm src/*.sym

.PHONY: gfx clean
