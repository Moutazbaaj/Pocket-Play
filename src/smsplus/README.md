# SMS Plus in Pocket Play

The Master System and Game Gear hardware emulation comes from [SMS Plus in Retro-Go 1.46, revision 856e2f7](https://github.com/ducalex/retro-go/tree/856e2f7144935c7a9e230d3a0709218f823411ba/retro-core/components/smsplus). See the [GPL notices](COPYING), original source headers, and [upstream credits](CREDITS). The Arduino adaptation omits the ColecoVision BIOS and the unused FM implementations. It replaces the bundled Z80 and PSG with the following sources.

- [Nicolas Allemand's Z80](https://github.com/superzazu/z80/tree/d64fe10a2274e5e40019b1086bf7d8990cbc5f23), [MIT license](cpu/LICENSE). `cpu/SegaCpu.c` connects it to the SMS Plus memory bus, interrupt lines, cycle budget, and snapshots.
- [Mitsutaka Okazaki's emu76489 1.22](https://github.com/digital-sound-antiques/emu76489/tree/c0fa097060e022db237163d79704025435042997), [MIT license](sound/LICENSE). The sound bridge feeds separate left and right PSG samples into the Pocket Play audio path.

`PocketSega.c` owns the ROM buffer, session lifetime, and cart saves. Large video lookup tables live in PSRAM and are released when the session closes. The adapter exposes the active viewport and RGB565 palette to Pocket Play's display task. Sega snapshots use Pocket Play's state format because the CPU and PSG differ from Retro-Go's.
