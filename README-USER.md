# Pocket Play player guide

Pocket Play is a DIY handheld by Moutaz Baaj. It plays Game Boy, Game Boy Color, Game Boy Advance, NES, Sega Master System, Game Gear, and Genesis / Mega Drive games from a microSD card. It also runs Doom using your own IWAD and has a music player, photo viewer, clock, mini games, onboard LED flashlight, adjustable display brightness, and console sleep. GBA and Genesis performance varies by game.

- **Set up** · [First USB install](#download-and-install) · [microSD card](#set-up-the-microsd-card) · [USB controls](#use-a-usb-keyboard-or-gamepad) · [Bluetooth controllers](#pair-a-bluetooth-controller)
- **Play and explore** · [Home](#home-and-built-in-games) · [Games and saves](#play-a-game) · [Doom](#play-doom) · [Music](#play-music) · [Photos](#view-photos) · [Clock](#clock) · [Flashlight](#use-the-flashlight)
- **Transfer and update** · [Wi-Fi file transfer](#upload-files-over-wi-fi) · [Wi-Fi firmware update](#update-firmware-over-wi-fi) · [microSD firmware update](#update-firmware-from-microsd)
- **Apps and settings** · [Install applications](#install-and-launch-applications) · [Build your own app](docs/APP_SDK.md) · [Display and sound](#change-the-display-and-sound) · [Diagnostics](#record-a-diagnostic-log) · [Sleep and wake](#sleep-and-wake)

## Download and install

If your Pocket Play already offers a firmware updater, use the app download ending in `.ino.bin`. For a blank board, a board without compatible update slots, or recovery, use USB. [Wi-Fi update](#update-firmware-over-wi-fi) · [microSD update](#update-firmware-from-microsd)

1. If a release provides a USB package for your hardware, follow its flashing instructions. To build from source, download a release's source ZIP or use **Code → Download ZIP** on the repository. [GitHub Releases](https://github.com/Moutazbaaj/Pocket-Play/releases) · [USB package and recovery](BUILDING.md#package-firmware-for-usb-and-wi-fi)
2. To install from source, extract the ZIP, rename the sketch folder to `PocketPlay`, and open `PocketPlay.ino` in Arduino IDE.
3. In **Boards Manager**, select **esp32 by Espressif Systems** and install **3.3.2**, the version used for the current reference build and its Wi-Fi Transfer. Use this same board package version on each computer you build from. [Espressif 3.3.2 release](https://github.com/espressif/arduino-esp32/releases/tag/3.3.2)
4. For the reference **ESP32-S3-WROOM-1-N16R8** board, select **ESP32S3 Dev Module**, **16 MB flash**, **OPI PSRAM**, and the partition scheme **16M Flash (3MB APP/9.9MB FATFS)**. [Full board settings](BUILDING.md#build-and-upload) · [Wiring](docs/HARDWARE.md)
5. Connect the handheld through its USB-to-UART port, choose that serial port, and click **Verify**, then **Upload**. The startup log should show **16 MiB flash** and **8 MiB PSRAM** at **115200 baud**.

## Use a USB keyboard or gamepad

Plug a USB boot-compatible keyboard or standard USB HID gamepad into the handheld’s **native USB / OTG** port using a host adapter and a setup that supplies USB VBUS. Use arrows to move, **Z / X** for A / B, **I / U** for Start / Select, **Q / E** for L / R, and **Escape** for Menu. On generic HID gamepads, use the D-pad or stick for direction and press Start + Select together for Menu. Test input in **Settings → Device Tools → Button Test**. Keep Auto Sleep off when using the handheld without physical Start and Select buttons. [USB setup and full controls](docs/HARDWARE.md#usb-keyboard-and-gamepad)

## Pair a Bluetooth controller

From Home, open **Settings → Connectivity → Bluetooth**, choose **Enable on Restart**, and press **A** on **Restart Console**. When Home returns, open Bluetooth again. Put the controller in pairing mode and choose **Scan for Controllers**. When scanning finishes, highlight your controller and press **A**. Once it connects, use it to browse the launcher and play. On a compatible Xbox controller, **View** is Select, **Menu** is Start, and the **Xbox** button opens Pocket Play’s menu.

**Restart Console** is always visible. Choose **Enable on Restart** first to schedule Bluetooth startup.

Pocket Play supports compatible Xbox and generic Bluetooth LE HID controllers. Sony DualShock and DualSense use Bluetooth Classic, which the ESP32-S3 cannot pair with directly. Pocket Play remembers the last bonded pad and tries to reconnect when Bluetooth starts. [Bluetooth compatibility](docs/HARDWARE.md#bluetooth-controllers)

One restart is expected when you enable Bluetooth. If Home keeps restarting or Bluetooth is off when you return, update the firmware and verify that the startup banner shows the expected **8 MiB PSRAM** on the reference board. Hold the physical **Start + Select** buttons during power-on or reset to clear the Bluetooth toggle. Release them at Home. Serial Monitor at **115200 baud** shows `Bluetooth: startup complete` when the radio is ready. [Bluetooth troubleshooting and serial output](docs/HARDWARE.md#troubleshoot-bluetooth-startup)

## Upload files over Wi-Fi

1. Insert the microSD card for game and media uploads. Open **Settings → Connectivity → Wi-Fi**. Use **Network Name** and **Password** to change the defaults if you like. Use the D-pad and **A** to type, **B** to erase, and **Save** on the keyboard to keep the value. The name can be up to 32 characters and the password must be 8–63 characters.
2. Select **Enable on Restart**, then **Restart Console**. The transfer screen shows the network name, password, and browser address. Join that Wi-Fi network on your computer or phone. Stay connected if your device reports no internet access.
3. Type **`http://192.168.4.1/`** in your browser’s address bar. In **Upload to microSD**, choose files or drop them onto the page, then select **Upload**. Pocket Play sorts uncompressed ROMs into their system folders and puts supported Doom IWADs in `doom/`, music in `music/`, and photos in `photos/`. If a file already exists in its destination, the page reports it and keeps the existing copy.
4. Wait for the upload results. When the server is idle, select **Restart console** in the browser or press **B** or **Menu** on the handheld. Pocket Play reopens the launcher with Wi-Fi off and indexes the card. Transfer mode also exits after ten minutes of inactivity.

Bluetooth stays off during transfer mode and resumes its saved setting on the next normal boot. Doom uploads are limited to 32 MiB per file, photos to 6 MiB, and music to 128 MiB. [Supported file formats](#set-up-the-microsd-card)

If a transfer is interrupted, reopen Wi-Fi transfer mode before retrying. It clears the unfinished temporary upload on startup. If the hotspot is missing, your device has no IPv4 address, or the page fails to load after a new USB flash, check the board package and settings used for that build. [Wi-Fi troubleshooting](BUILDING.md#troubleshoot-wi-fi-and-firmware-updates)

## Install and launch applications

1. Get a Pocket Play package ending in `.ppapp`. You can try [Button Lab](examples/pocketplay-sdk/button-lab.ppapp).
2. In the Wi-Fi page's **Applications** section, select the package and click **Install application** to install it on microSD. Choose **Restart console** when it finishes. You can also choose **Save to microSD** to install it later.
3. If you saved it for later or copied it to `/app-install/` or the card root, open **Settings → About → Install Application**, highlight the package, and press **A**.
4. On Home, open **Applications**, select your app, and press **A**. Press **Menu** to return to the list.

You can make your own small games or tools with the [app SDK](docs/APP_SDK.md).

## Update firmware over Wi-Fi

1. Get the release’s Pocket Play app image ending in `.ino.bin` for your wiring. If you compiled the firmware yourself, use **Sketch → Export Compiled Binary** to get `PocketPlay.ino.bin`. [Build and packaging](BUILDING.md#package-firmware-for-usb-and-wi-fi)
2. Start a Wi-Fi transfer session and open the browser page. Under **Firmware update**, choose the file, select **Install firmware**, and confirm.
3. Keep the console powered and stay connected. The browser’s bar shows upload progress. The current firmware also shows **Installing update** and **Keep the console powered** in the center of the console above a bar showing flash progress.
4. When the browser says **Firmware ready**, choose **Restart console**. Pocket Play starts the new firmware with Wi-Fi off.

Direct browser updates work without a microSD card. Both update methods require compatible dual 3 MiB app slots and the existing firmware’s update feature. If you see a message that USB is required, use the reference partition scheme and install over USB once. If Pocket Play cannot start after updating, use the USB recovery package. [First install](#download-and-install) · [USB recovery](BUILDING.md#package-firmware-for-usb-and-wi-fi)

## Update firmware from microSD

1. Copy the Pocket Play `.ino.bin` app image to `/updates/` or the root of the microSD card. To send it wirelessly, choose the image under **Firmware update** on the Wi-Fi page and select **Save to microSD**. Pocket Play creates `/updates/` if needed and saves the file for later installation. Choose **Restart console** to return to Home.
2. Quit any game and stop any diagnostic recording. From Home, open **Settings → About → Update Software**. Highlight the image and press **A**. Press **A** again to install or **B** to cancel.
3. Keep Pocket Play powered and leave the card inserted. The centered progress bar reaches 100% when the firmware is ready. Pocket Play then restarts automatically.

Use the `.ino.bin` app image for your hardware. USB bootloader, partition, and merged flash images are for USB recovery. [USB packaging and recovery](BUILDING.md#package-firmware-for-usb-and-wi-fi)

## Play Doom

Copy your `doom1.wad`, `doom.wad`, or `doom2.wad` into `/doom` on microSD, or upload it from the Wi-Fi transfer page. Doom needs **8 MiB PSRAM**. Open **Doom** from Home and choose your IWAD. The console restarts into the selected game.

Use the D-pad to move and turn, **A** to fire, **B** to open doors and use switches, **L / R** to strafe, **Select** for the map, and **Start** to cycle weapons. Use **Menu** to access Doom’s save, load, and quit options, and **Start** to select or confirm. **Quit Game** restarts Pocket Play into Home. Doom uses the handheld’s physical controls. Sound effects follow the console’s sound setting, and saves live under `/doom/saves/` in a folder named after the IWAD.

## Set up the microSD card

Use a **FAT32** card. Create the folders below on the card and copy in uncompressed files you are authorized to use. Back up any files you want to keep before formatting a card.

| Folder on the microSD card | Files |
| --- | --- |
| `roms/gb/` | Game Boy `.gb` |
| `roms/gbc/` | Game Boy Color `.gbc` |
| `roms/gba/` | Game Boy Advance `.gba` |
| `roms/nes/` | Nintendo Entertainment System `.nes` cartridges in iNES format |
| `roms/sms/` | Sega Master System `.sms` |
| `roms/gg/` | Sega Game Gear `.gg` |
| `roms/genesis/` | Sega Genesis / Mega Drive `.md`, `.gen`, `.smd`, and `.bin` up to 6 MiB. Use `.md`, `.gen`, or `.smd` over Wi-Fi. |
| `doom/` | Doom IWADs named `doom1.wad`, `doom.wad`, or `doom2.wad`, up to 32 MiB each. |
| `updates/` | Compatible Pocket Play app images ending in `.ino.bin`, up to 3 MiB each. |
| `app-install/` | App packages ending in `.ppapp`, ready to install from About. |
| `apps/` | Installed apps shown in Applications on Home. |
| `music/` | `.mp3` and 16-bit PCM `.wav` files. Album subfolders are supported. |
| `photos/` | JPG, JPEG, PNG, BMP, GIF, and PSD images. Album subfolders are supported. |

Add your own game and music files. The firmware comes with an open-source GBA BIOS replacement. NES, Master System, and Game Gear launch from their cartridge files. NES 2.0 and Famicom Disk System files fall outside the supported NES formats.

## Home and built-in games

Use the D-pad to browse Home and press **A** to open the selected item. **Continue**, **Favorites**, and **Recently Played** come first, followed by the seven console libraries. Continue browsing for **Doom**, **Mini Games**, **Music**, **Flashlight**, **Photos**, **Clock**, **Applications**, and **Settings**.

Open **Settings → Display → Home Style** and press **Left / Right** or **A** to choose a layout. Your choice is saved for the next time you turn on Pocket Play.

- **Grid** is the default and uses three columns of equally sized cards. Use **Up / Down** to move between rows and **Left / Right** to move between cards.
- **Carousel** puts the selected item in the center. Use **Left / Right** to browse.
- **List** shows compact rows. Tap or hold **Up / Down** to scroll through them.

**Mini Games** includes **Tile Puzzle**, **Timing**, and **Snake** and works without microSD. In Tile Puzzle, use the D-pad to move the empty space and **Start** to shuffle again. In Timing, press **A** when the marker reaches the target, then press **A** for the next round. In Snake, steer with the D-pad, eat fruit to grow, and avoid the walls and your tail. Use **Start** to restart or **B** to go back.

## Clock

Open **Clock** on Home. Press **Menu** to choose a Digital, Analog, or Minimal clock face. Choose **Time Format** for 24-hour time or 12-hour time with AM/PM. Set the date and time with **Left / Right**. In 12-hour mode, press **A** on **Hour** to switch between AM and PM. Highlight **Set Date / Time** and press **A** to apply. The time also appears beneath the battery indicator wherever it is shown.

For a countdown, set **Timer Length** (1–99 minutes), select **Countdown**, and press **A** to start or stop it. The display shows **Timer Done** when it reaches zero. The clock face stays on while it is displayed. The console saves your clock format, face, and timer length. Set the date and time again after disconnecting power.

## View photos

Open **Photos** from Home. Press **Menu** and choose **Browser Layout** to switch between **List**, **Carousel**, and **Grid**. Pocket Play remembers the selection. Press **A** to open an album or image, **Left / Right** in the viewer to move between photos, and **B** to go back. Press **Select** in the album browser to refresh the folder.

To delete an image, highlight it or open it, then choose **Menu → Delete Photo**. Select **Yes** to permanently remove that image file. The prompt starts on **No**.

Photos fits images to the screen while keeping their proportions. GIFs show their first frame, and PSDs show the saved composite image. Use **320 × 240** copies for the fastest viewing, especially for large phone photos.

## Play a game

1. Insert the card and power on. The boot splash shows progress as Pocket Play indexes your game library.
2. On Home, choose the console for your game. Press **A** to open that system's list and again to start a game. Tap **Up / Down** to move one row or hold either button to keep scrolling.
3. Press **B** to return from a game list. Press **Select** from Home or a game list to rescan the card after adding files.
4. Press **Menu** during gameplay for **Save state**, **Load state**, **Display size**, **Volume**, or **Quit to game list**. Save states are stored on the card next to the game. Use **Save state** before switching off if you want to return to the same point later.

Use **Left / Right** on **Save state** or **Load state** to choose one of three slots, then press **A**. Check the slot number and choose **Yes** to proceed. The prompt starts on **No** and warns before an overwrite. Older Pocket Play save states are in **Slot 1**. NES and Sega snapshots use Pocket Play’s own format.

For cover art, put uncompressed **24-bit BMP** files in the system’s `imagesets` folder, such as `/roms/gbc/imagesets/`. If the ROM is `008 Super Mario Bros. Deluxe.gbc`, use `008 Super Mario Bros. Deluxe.gbc.bmp` or `008 Super Mario Bros. Deluxe.bmp`. Stop scrolling on a game to see its cover. You can keep browsing while a cover loads. Moving to another game cancels the earlier preview. New covers wait until background music pauses or stops.

**Continue** on Home opens your last game and loads the most recently saved slot when one exists. Press **Right** on a game to add or remove its star. **Favorites** and **Recently Played** are on Home and use the current microSD card.
Press **Menu** on a highlighted game in a console library, Favorites, or Recently Played. The menu uses the console’s accent color, just like its in-game pause menu.

- **Game Settings** lets you configure display size, game sound, and GBA frameskip. Choose **Save Profile** to keep changes or set **Use Profile** to **Default** to use the global settings.
- **Rename Game** opens a keyboard for changing the filename on the card. Use the D-pad to move, **A** to type, **B** to erase, and **Menu** to cancel. Choose **Save** on the keyboard when done. The extension stays the same and saves and artwork remain attached to the title.
- **Delete ROM** permanently deletes the ROM when you select **Yes**. The prompt defaults to **No**. Saves, profiles, and artwork stay on the card.

In Genesis games, use **R** or **Select** for the C button. [Genesis setup and compatibility](src/mdcore/README.md)

## Play music

Open **Music** from Home. Tap or hold **Up / Down** to browse the library. Press **A** to open an album folder or play a track, **Start** to pause or resume, and **B** to go back. Press **Menu** from the library or Now Playing to open Music Options. Large folders open in small batches while the current track keeps playing. Press **B** while a folder is loading to cancel. The lower row of Now Playing is **Vol −**, **Vol +**, then **Repeat**.

| Option | When it appears | What it does |
| --- | --- | --- |
| **Up One Folder** | Inside an album or disc folder | Opens the parent folder while the current track continues. |
| **Hide Music** | At `/music` with Background Playback on | Returns to Home while music continues. |
| **Close Options** | At `/music` with Background Playback off | Returns to the music screen that opened Options. |
| **Exit Music** | Anytime in Music Options | Stops playback and returns to Home. |

Choose **Rescan Library** in Music Options after adding music to the card. Choose **Screen Off** or set **Auto Screen Off** to put the display to sleep during playback. Press any button to wake the display during playback. While background playback is on, the visible Music item on Home displays the current song. In Carousel, select Music to see it. Long titles scroll from right to left on Home and in Music. In **Music Options**, use **Sleep Timer** to stop music after 15, 30, or 60 minutes, **Visualizer** for the stereo level bars, and **Music RGB** for the optional sound-reactive onboard LED.

## Use the flashlight

Open **Flashlight** from Home and press **Start** to toggle the light. Choose **Color Flash** to blink or **Color Breathe** to fade the LED in and out. Use **Left / Right** on **Effect Speed** to choose **Slow**, **Normal**, or **Fast**. Press **B** or **Menu** to save your choice and return to Home.

## Change the display and sound

The top-right battery icon shows a smoothed estimate in menus, including Music and the game pause menu. Red means **20% or less**. Brief dips during loud playback are filtered, and sustained low voltage still triggers the red warning quickly. An empty icon with `--%` means the battery reading is unavailable. [Battery indicator wiring](docs/HARDWARE.md#battery-indicator)

Choose **Settings** on Home or in the game pause menu. Pressing **Menu** on Home also opens Settings. Press **A** or **Right** to open a group. Use **Left / Right** to adjust values marked with arrows, such as `< 50% >`. Press **B** or **Menu** to go back one level.

- **Display** contains **Display Brightness**, **Theme**, **Home Style**, and **TFT Speed**. Choose **Light** or **Dark** under **Theme**. The choice is applied immediately and saved. Brightness changes the backlight from 20% to 100% in 20-point steps and saves the selected level. For a DIY build, wire the TFT’s **LED / BL** backlight control to **GPIO38** if the pin accepts a 3.3 V logic signal. If the LED pad supplies power to the backlight, use a backlight switch controlled by GPIO38. [Display wiring](docs/HARDWARE.md#display-storage-and-audio)
- **Sound** contains **Game Sound** and **Volume**. The volume also applies to the music player. Audio output requires a connected speaker and I²S amplifier. If loud playback causes screen flicker, check the amplifier power connection. [Amplifier wiring](docs/HARDWARE.md#amplifier-power-and-display-flicker)
- **Emulation** contains the display sizes, GBA Frameskip, and GBA Transfer. **GB / NES / Sega Size** offers **Native**, **Fit**, and **Full**. For a slow GBA game, try **GBA Display → Native** and **GBA Frameskip → Auto**.
- **Power** contains **Sleep Console** and **Auto Sleep**. [Sleep and wake](#sleep-and-wake)
- **Connectivity** contains Bluetooth pairing and Wi-Fi file transfer. **Device Tools** opens the hardware tests. **About** opens **About Pocket Play**, **Diagnostic Log**, **Install Application**, and **Update Software**. Open **About Pocket Play** to see the software version. Its **Build versions** page also shows the build ID and the installed Arduino-ESP32 and ESP-IDF versions.

In **About Pocket Play**, scroll to **Network addresses** for the factory, Wi-Fi client, Wi-Fi hotspot, and Bluetooth LE MAC addresses. **Device stats** shows the chip revision and hardware details.

## Device Tools

Open **Settings → Device Tools** to test buttons, display colors, the I²S speaker, and the onboard RGB LED. The button test highlights held inputs and shows their GPIO assignments. Hold **Menu** or **B** for about a second to leave it. Use **A** to cycle solid display or LED colors. **Battery / SD** shows the latest averaged voltage and the smoothed charge estimate when the divider is wired, plus the card’s total capacity, remaining free space, and ROM counts.

## Record a diagnostic log

1. Open **Settings → About → Diagnostic Log**. Use **Left / Right** on **Duration** to choose 30 seconds or 1–5 minutes. Pocket Play remembers this setting.
2. Select **Start Capture**. Use **B** to leave the menu and run the test in Music, a game, or another app. Capture ends when the timer runs out, you select **Stop Capture**, or you put the console to sleep manually.
3. Open **History** and press **A** on a session. **Up / Down** steps between samples. **Left / Right** jumps ten samples. Each capture is saved in `/logs` on microSD, with the date from the Clock app if set.
4. Type `log` in Arduino Serial Monitor at **115200 baud** to print the latest completed session as CSV.

Samples include battery voltage in **mV**, the change from the first valid reading, the smoothed charge estimate, activity, playback state, volume, backlight level, and the flashlight/RGB state. The Diagnostic Log menu shows the same **Voltage now** reading as **Battery / SD**. In **History**, that label shows the voltage saved with the selected sample.

## Sleep and wake

- Hold the physical **Start + Select** buttons together for about **0.6 seconds** to sleep from Home, normal apps, or an emulated cartridge game. Release both once the screen turns off. Wi-Fi transfer mode and Doom use dedicated sessions with their own exit controls.
- **Settings → Power → Sleep Console** starts light sleep when you press **A**. You can also use the **Sleep console** shortcut directly in the pause menu.
- **Settings → Power → Auto Sleep** offers **Off**, **30 seconds**, **1 minute**, **2 minutes**, and **5 minutes**. Use **Left / Right** to choose a delay. It starts at **Off** and remembers your choice. The timer runs in idle menus and while a game is paused. Controls restart it. Gameplay, music playback, the flashlight, a clock face, an open mini game, an RGB LED test, and diagnostic recording keep the console awake.

To wake Pocket Play from light sleep, **hold Start and Select together**, then release both. A game resumes if you put it to sleep during gameplay or used the pause-menu **Sleep console** action. From other screens, you return to where you left off.

## If a game does not appear or load

- Check that the card is FAT32 and the ROM is uncompressed in the correct folder. Press **Select** to rescan.
- If scans give different results or several games fail to load, power off and check the card reader connections. Try a known-good card with one game to narrow down the issue.
- If the N16R8 startup log shows `PSRAM: 0 MiB`, verify the **OPI PSRAM** board setting and upload again.

See the [hardware guide](docs/HARDWARE.md) for wiring and the [technical reference](docs/TECHNICAL_REFERENCE.md) for credits and performance notes. Pocket Play is an independent project.
