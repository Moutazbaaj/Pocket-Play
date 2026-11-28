# ESP32-S3 IPC stack

Pocket Play builds this copy of Espressif's IPC implementation on the dual-core ESP32-S3 with Arduino-ESP32 3.3.2 and NimBLE enabled. The file comes from the ESP-IDF revision bundled with that board package. [Original source](https://github.com/espressif/esp-idf/blob/07e9bf497094c7efa63efd42b28021e2cdd8c235/components/esp_system/esp_ipc.c) · [Apache 2.0 license](LICENSE)

The reference SDK gives each IPC task a 1 KiB stack. The v1.0.2 Bluetooth startup trace showed `ipc0` reaching its stack canary while `btdm_intr_alloc` called `esp_intr_alloc` and the heap allocator. On the core configured for the Bluetooth controller, this copy raises the IPC stack to at least 3 KiB. The other core uses the SDK size. Other targets and Arduino core versions use their own SDK implementation.
