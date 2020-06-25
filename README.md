# Mini64

Mini64 is a fork of Mupen64Plus-Libretro-NX that aims to target low power systems and to simplify use. Mini64 is a libretro core that emulates the Nintendo 64 system, and must be used with libretro frontends such as [RetroArch](https://github.com/libretro/RetroArch) or [sdlarch](https://github.com/heuripedes/sdlarch).

## Notable Changes

- Simplified Makefile, and added `make help` option to make it clear what options are available to the builder. This could mean that certain targets will require addition configuration to CFLAGS, etc.
- Removed all plugins except for GLideN64 and HLE.
- Added logging fallback for libretro frontends which do not implement a log callback environment.
- Integrated ROM settings properly. [More Info](#ROMSI)
- Remove MSVC specific project files and libraries. Builds with MSVC *should* be possible without workarounds. I aim to target MSVC 2017.
- Remove files and logic not necessary for a libretro core (such as screenshot).
- Fix building without texture pack support (GLideNHQ).
- Remove 64DD emulation. If you would like to play games of this failed system, use another emulator.
- Fix some compiler warnings.
- Remove Project64 Save State support. Loading Save RAM should still be supported.
- Fix building with link-time optimisation (LTO).
- Add automatic Raspberry Pi target detection on build.
- Remove texture dumping capability. It is not expected that the average user would require this feature. Use another emulator if you wish to dump textures.

### <a name="ROMSI"></a> ROM Settings Improvement

Previously, a large ini file was used to store settings required for certain games to function properly. This also included a "good name" which stored the full titles of game and region as a string for each and every game. This good name was only printed to the log output, so the player would never see this whilst playing their game. Furthermore, the ini file was stored as a large auto string, with entries within the file stored in a random order. This file added approximately 500 KiB to the output binary.

The data within this ini file was converted to a C structure, with the good name data field removed. Furthermore, the entries are now sorted by the CRC ROM header hash, allowing for faster look-up times using binary search. These changes reduced the data within the ini file, to a structure that only required 15 KiB of storage.

The tool mupenini2dat was written for this purpose, and may be found within the tools folder.

## Future Changes

- Support for mTP64 texture packs.
- Add texture packs using libretro subsystem.
- Game Boy transfer pack libretro subsystem support.
- Improved user configuration settings.

## Upstream Projects

The following projects have been incorporated into this repository:

- [mupen64plus](https://github.com/mupen64plus/mupen64plus-core)
- [GLideN64](https://github.com/gonetz/GLideN64)
