# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an offline LCD controller/pendant for a CNC machine running GRBL v1.1. An Arduino Mega
drives a 20x4 I2C LCD, a rotary encoder with push button, and a microSD reader, and talks over
`Serial1` (pins 18/19) to a second Arduino running the actual GRBL firmware. There is no host PC in
the loop — the controller reads G-code files from the SD card and streams them to GRBL line by line.

The entire application lives in one file: `src/Codigo.ino` (~1000 lines, all globals and
functions in one translation unit — this is normal for Arduino sketches, not a code smell to fix).

## Build / upload

This is a PlatformIO project (config at `platformio.ini`, target `megaatmega2560`,
`framework = arduino`). From the repository root:

```
pio run                 # build
pio run -t upload       # build and flash to the Mega
pio run -t clean
```

The project can also be opened directly in the Arduino IDE (`src/Codigo.ino`); the required
libraries are vendored under `lib/` (Encoder, LiquidCrystal_I2C, SD) so no library manager
step is needed either way.

There is no test suite — this is single-file embedded firmware without a host-side abstraction to
unit test. Validate changes by building for `megaatmega2560` and, when possible, exercising the
actual menu flow on hardware.

## Hardware wiring (reference)

Defined at the top of `Codigo.ino`:
- SD card (SPI): CS=53, MOSI=51, MISO=50, CLK=52
- Rotary encoder: CLK=2, DT=3, SW(button)=4
- Jog buttons: X+=22, X-=23, Y+=24, Y-=25, Z+=26, Z-=27, spindle toggle=28
- LCD (I2C, addr 0x27, 20x4): SDA=20, SCL=21
- `Serial` (USB) is debug logging only; `Serial1` (pins 18/19) is the link to the GRBL board.

## Code structure and control flow

Everything is state-machine style, driven from `loop()`: it refreshes the status line every 250ms
and, when the encoder button is pressed, hands control to `menuP()`, the top-level menu. Each menu
function (`menuP`, `controlMenu`, `menuMoveAxis`, `setAxisToMove`, `settingMenu`, `fileMenu`) follows
the same pattern:
- Draws its own screen via `setTextDisplay(line1, line2, line3, line4)`.
- Runs its own blocking `while` loop polling `myEnc.read()` for rotation and `digitalRead(selectPin)`
  for the click, with a `timeExit`-based inactivity timeout that returns to the previous menu.
- `moveOption()` draws/moves the `=>` cursor for the currently highlighted line.

GRBL communication is centralized in a handful of functions — extend these rather than talking to
`Serial1` directly from a new menu:
- `sendCodeLine(line, waitForOk)` — writes a line to GRBL and, if `waitForOk`, blocks until an `ok`
  response is seen (via `checkForOk()`), while still servicing the display/menu so the UI doesn't
  freeze during long-running moves.
- `checkForOk()` — drains `Serial1`, watching for `ok` and for GRBL `error:5` (homing not enabled).
- `getStatus()` — sends `?`, parses the `<Idle|WPos:x,y,z|...>` status report into the globals
  `machineStatus`, `WposX`, `WposY`, `WposZ`.
- `updateDisplayStatus()` — renders those globals plus an elapsed-time counter to the LCD.

File sending (`sendFile`) streams a G-code file from SD line-by-line through `sendCodeLine`,
stripping unsupported/unsafe codes first via `ignoreUnsupportedCommands()` (e.g. `G28`, `G92`, tool
changes, comments) — GRBL 1.1 doesn't support all of these, and some (like `G92`) are deliberately
disabled here in favor of the controller's own zeroing (`G10 P0 L20 ...`). While a file is running,
`checkButtonSlect()` watches for the button to open `modMenu()` — a nested overlay for
Hold/Resume/Abort and live feed/spindle override (sent as raw GRBL real-time bytes, e.g. `0x90`–`0x9B`)
without interrupting the streaming loop.

Most inter-menu state (current option, encoder position, whether a mod overlay is open, etc.) is
kept in global variables rather than passed as parameters — when adding a new menu or overlay, follow
that existing convention rather than introducing a different state-management approach.

The serial baud rate to the GRBL board is persisted in EEPROM address 0 (set via `settingMenu()`,
which then soft-resets the Mega with `asm("jmp 0x0000")` to apply it).

## Other repo contents

- `Algorithms-Flow charts/` — PNG flowcharts documenting the control flow of individual functions
  (named after the function they document, e.g. `void sendFile().png`). Useful as a visual reference
  when reasoning about a function's logic, but not authoritative — the code is ground truth.
- `Schematic.png` — wiring schematic.
