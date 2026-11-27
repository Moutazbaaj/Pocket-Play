# Doom engine port

The launcher uses the GPL Doom engine from [rear16/ESP-DOOM at 92bd5c3](https://github.com/rear16/ESP-DOOM/tree/92bd5c3968197683041e7917e5f9e709164ba1fd), based on DoomGeneric, Chocolate Doom, and id Software’s Doom source release. See [LICENSE](LICENSE) and the notices in each source file.

Pocket Play supplies the input, display, I²S output, and microSD integration. The engine keeps its zone in PSRAM and returns to Home by restarting the console. The WADs, game artwork, and sound assets come from the player’s own IWAD.
