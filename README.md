# gbdk-hero

Game Boy (DMG) games written in C with [GBDK-2020](https://github.com/gbdk-2020/gbdk-2020).

| Folder | Game | ROM |
|---|---|---|
| `/` | Hero: a top-down, Zelda-style adventure | `hero.gb` |
| [`tether/`](tether/) | Tether: a deep-sea dive, guiding a diver on an air hose | `tether/tether.gb` |
| [`stealth/`](stealth/) | Stealth: a Shadow Tactics–style real-time tactics prototype | `stealth/stealth.gb` |

Tether and Stealth also have phone-playable browser prototypes in their `web/` folders.

## Building

With GBDK-2020's `lcc` on your PATH:

```sh
make                 # hero.gb
make -C tether       # tether/tether.gb
make -C stealth      # stealth/stealth.gb
```

The GitHub Actions workflow builds all three ROMs on every push to `main` and uploads them as
build artifacts.
