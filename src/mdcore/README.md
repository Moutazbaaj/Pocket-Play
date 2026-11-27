# mdcore in Pocket Play

The Genesis / Mega Drive core is based on [mdcore revision 3db9251](https://github.com/andrewnakas/mdcore/tree/3db92517338179ea7797af1559255ccecaffb237). It is distributed under the included [MIT license](LICENSE). Pocket Play uses the native C engine and its own launcher, input, display, audio and storage code.

The port allocates cartridge, machine, VDP and frame memory in PSRAM when a game opens. It renders RGB565 directly and presents the current field in interlaced modes. The adapter exposes a three-button pad, sends stereo PCM through the existing I²S path and stores battery SRAM and save states alongside the ROM.

Copy `.md`, `.gen`, `.smd` or `.bin` cartridges up to 6 MiB to `/roms/genesis/`. The Wi-Fi uploader accepts `.md`, `.gen` and `.smd`. Pocket Play’s A and B buttons map to A and B, and R or Select maps to C.

The upstream core has tests for Sonic the Hedgehog 1 and 2. Pocket Play’s frame rate and title compatibility require device testing. EEPROM saves, SVP, Sega CD and 32X are outside this core’s supported hardware.
