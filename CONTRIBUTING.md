# Contributing to Pocket Play

Bug fixes, emulator performance improvements, hardware compatibility reports, and documentation updates are welcome. Use GitHub Issues to report a problem or discuss a larger change. Small fixes can go straight to a pull request. For an overview of the firmware modules and emulator adapters, see the [source map](docs/TECHNICAL_REFERENCE.md#code-and-emulator-lineage).

## Open a pull request

1. Fork Pocket Play on GitHub. Clone your fork into a folder named `PocketPlay` so Arduino IDE can open `PocketPlay.ino`.

   ```sh
   git clone https://github.com/YOUR-USERNAME/YOUR-FORK.git PocketPlay
   cd PocketPlay
   git switch -c fix/describe-the-change
   ```

2. Make one focused change. Follow the [build instructions](BUILDING.md#build-and-upload) for firmware changes. For documentation changes, check the instructions and links.
3. Use a commit message that describes the result, such as `Fix MP3 seeking after pause`. Push the branch to your fork.

   ```sh
   git add path/to/changed-file
   git commit -m "Fix MP3 seeking after pause"
   git push -u origin fix/describe-the-change
   ```

4. On GitHub, open a pull request from your fork's branch against Pocket Play's `main`. Describe what changed and how you tested it, and link any related issue. [GitHub flow](https://docs.github.com/en/get-started/using-github/github-flow)

## Share test results

- For firmware or hardware changes, include your ESP32 board, display, Arduino-ESP32 version, and any relevant settings. State whether you compiled the sketch, tested it on the device, or both.
- For a bug report, include the steps, expected result, actual result, and relevant serial output.
- For GBA performance changes, compare the same scene and settings and report both emulated FPS and display FPS when available. [Performance counters](docs/TECHNICAL_REFERENCE.md#read-the-performance-log)

## License and assets

Submit only changes you have permission to contribute. Original changes to Pocket Play code are submitted under GNU GPL version 2 or later. Preserve the applicable notices in vendored code and identify the source, license, and version of any third-party code or data you add. [Project license and upstream credits](README.md#credits-and-licenses)

Keep commercial game ROMs, proprietary BIOS dumps, save data, music, and artwork you lack permission to redistribute out of commits, issues, and pull requests. A reproduction can describe the game, region, and save type without attaching the files.
