# Pocket Play app SDK

SDK **1** lets you write small games and tools that run from microSD. Use any plain-text editor to create a Lua package. You can install and run it on a Pocket Play with SDK 1 without building the firmware.

[Write your first app](#write-your-first-app) · [Examples](#test-applications) · [Install an app](#install-and-launch) · [API reference](#app-lifecycle) · [Troubleshooting](#troubleshoot-an-app)

## Write your first app

Write a Lua source file with the two header lines below and save it with a `.ppapp` extension. A package is one plain-text file of up to **16 KiB**. Its filename is the app ID, using 1–32 lowercase letters, digits, underscores, or hyphens and starting with a letter or digit. The display title accepts up to 32 printable ASCII characters.

```lua
-- PocketPlay SDK 1
-- Title: Hello Pocket Play

local background = app.rgb(14, 24, 30)
local white = app.rgb(255, 255, 255)

function draw(full)
  if full then
    app.clear(background)
    app.text(16, 30, "Hello Pocket Play", white, background, 2)
  end
end
```

1. Save the example as `hello.ppapp` in UTF-8 without a byte order mark. Keep both header lines at the top.
2. With a card reader, copy the file to the microSD root or `/app-install/`. On Pocket Play, open **Settings → About → Install Application** and select **Hello Pocket Play**. For the browser workflow, see [Install and launch](#install-and-launch).
3. Open **Applications** on Home and launch **Hello Pocket Play**. The app should show its greeting. Press **Menu** to exit.
4. Change the greeting in your editor and reinstall the package. For input, tones, and persistent state, start with [Button Lab](../examples/pocketplay-sdk/button-lab.ppapp).

## Test applications

- [RGB Mixer](../test-applications/rgb-mixer.ppapp) previews RGB colors and saves a swatch.
- [Unit Converter](../test-applications/unit-converter.ppapp) converts temperature, length, and weight.
- [Block Catch](../test-applications/block-catch.ppapp) is a falling-block game with a saved high score.

## Install and launch

- On the Wi-Fi Transfer page, choose a package under **Applications**. **Install application** writes it to `/apps/` on microSD. Restart from the browser, then launch it under **Applications** on Home.
- **Save to microSD** on the Wi-Fi page stores the package in `/app-install/`. You can also copy it to that folder or to the card root with a card reader. From the console, choose **Settings → About → Install Application** and press **A** on the package. The installer copies it to `/apps/` and keeps the original. Restarting is unnecessary after a console install.
- Press **Menu** to exit a running app. Press **Select** in Applications or Install Application to rescan. The list shows up to 96 packages. To replace a development build, remove its old `.ppapp` from `/apps/` using a card reader, then install the new one under the same ID. The saved record stays with the ID.

## App lifecycle

| Function | When Pocket Play calls it |
| --- | --- |
| `init()` | Optional. Initialize app state and read a saved record. |
| `update(dt, pressed, held)` | Optional. Runs about every 33 ms when time permits. `dt` is elapsed milliseconds capped at 100. `pressed` includes new button presses since the previous update. `held` contains the current buttons. |
| `draw(full)` | Required. Called after initialization, after each update, and when the display wakes. Draw the whole view when `full` is true. Redraw only changed areas on other frames. |

The canvas is **320 × 224**. Pocket Play reserves the bottom 16 pixels for its exit hint. Buttons from the handheld, USB HID, and compatible Bluetooth LE controllers share the same masks. Test a new A press with `(pressed & app.A) ~= 0`. The host handles **Menu**.

| API | Behavior |
| --- | --- |
| `app.SDK`, `app.WIDTH`, `app.HEIGHT` | SDK version and canvas size. |
| `app.UP`, `app.DOWN`, `app.LEFT`, `app.RIGHT`, `app.A`, `app.B`, `app.START`, `app.SELECT`, `app.L`, `app.R` | Button masks. |
| `app.rgb(r, g, b)` | Converts 0–255 components to RGB565. |
| `app.clear(color)` | Fills the app canvas. |
| `app.fill(x, y, w, h, color)` | Draws a rectangle clipped to the canvas. |
| `app.text(x, y, text, fg, bg[, scale])` | Draws up to 40 bytes of text using the built-in 8 × 8 font, at scale 1–3. Supply the foreground and background colors in RGB565. |
| `app.sprite(x, y, w, h, pixels)` | Draws a sprite up to 32 × 32 inside the canvas. Supply one little-endian RGB565 value per pixel in a Lua string. |
| `app.tone(hz, ms)` | Queues a tone from 80–4000 Hz for 10–250 ms. Returns whether it was queued. Uses **Settings → Sound → Game Sound** and the system volume. Background music yields while an app is open. |
| `app.load()` | Returns the app's single saved record as a string, or `nil`. Read it at startup. |
| `app.save(data)` | Saves up to 256 bytes to `/app-data/<id>.sav` and returns success. Save after an event. Calls are limited to one save attempt per second and one storage operation per callback. |

Apps use Lua 5.4 syntax, `math`, and the base functions `assert`, `error`, `ipairs`, `next`, `pairs`, `select`, `tonumber`, `tostring`, and `type`. The runtime loads one source app into a Lua heap limited to 96 KiB of PSRAM. Apps use the APIs above for hardware and storage access. Each callback has an execution and drawing budget. An app that fails returns you to Applications. Use Serial Monitor at 115200 baud for the error message.

## Troubleshoot an app

| What you see | What to check |
| --- | --- |
| The package is missing from Install Application | Confirm the lowercase filename and `.ppapp` extension, both SDK header lines, and the 16 KiB limit. With the file in the card root or `/app-install/`, press **Select** to rescan. |
| The installer keeps the existing app | Remove the installed file from `/apps/` and install the new one. Keep the same filename to use the existing save in `/app-data/`. |
| The app exits on launch | Open Serial Monitor at **115200 baud** and try launching again. Check the reported Lua error, including whether the package defines `draw(full)`. |
| A large redraw exceeds the drawing budget | Use a full redraw for `draw(true)`. On other calls, repaint only the regions that changed. |

## Native C++ contributions

For a game bundled in firmware, use [AppSdk.h](../AppSdk.h) for drawing and the common input snapshot. [Native Counter](../examples/pocketplay-sdk/NativeCounter.cpp) demonstrates opening, updating, drawing, and closing a small view. Copy the example and its [header](../examples/pocketplay-sdk/NativeCounter.h) into the sketch folder to adapt them. Register a view in `PocketPlay.ino`, and call it from the Arduino main loop. [SnakeGame.cpp](../SnakeGame.cpp) shows how a bundled game refreshes just the board cells that changed. Build the whole sketch with the reference ESP32 core and settings. [Build guide](../BUILDING.md#build-and-upload)
