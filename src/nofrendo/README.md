# Nofrendo in Pocket Play

Nofrendo 3.0 by Matthew Conte and contributors, imported from [Retro-Go 1.46, revision 856e2f7](https://github.com/ducalex/retro-go/tree/856e2f7144935c7a9e230d3a0709218f823411ba/retro-core/components/nofrendo). See [credits](CREDITS), the source headers, the upstream [COPYING](COPYING), and the [GNU Library GPL v2 text](COPYING.LIB) identified by the original Nofrendo source notices.

`PocketNes.c` connects the core to the shared cartridge interface. The Arduino port uses relative includes, ESP32 CRC acceleration, and recoverable allocation failures. It opens raw iNES cartridges with supported mappers from `/roms/nes`. The loader checks lengths before handing ROM data to the core. Test ROMs and BIOS files are excluded from this import.
