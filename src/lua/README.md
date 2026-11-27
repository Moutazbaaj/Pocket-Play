# Lua in Pocket Play

The app runtime embeds selected Lua 5.4.9 sources under the included MIT license. Pocket Play builds the core, auxiliary library, base library, and math library. The runtime exposes a subset of base functions and the app API. The rest of the upstream standard libraries and command-line programs are omitted.

The local configuration enables 32-bit Lua numbers and limits Lua's nested C calls to 32 to fit the firmware task stacks. The firmware also bounds the loaded script size, per-app Lua heap, and execution and drawing work. [Lua 5.4.9 source and SHA-256](https://www.lua.org/ftp/) · [Local license](LICENSE)
