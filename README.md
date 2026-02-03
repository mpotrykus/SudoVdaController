# SudoVdaController

SudoVdaController is a C++ project that provides a controller and utilities for managing virtual displays using the Sudovda virtual display driver. It includes helper utilities for configuration, display topology, HDR, and display modes.

## Purpose

- Create, remove and manage virtual displays programmatically
- Apply display modes (resolution, refresh rate) and set primary displays
- Enable/disable HDR on supported virtual displays
- Persist and merge display topology/configuration via a config store

## Requirements

- SudoVDA virtual display driver installed

## Usage

### CLI

The command-line client supports the following verbs:

- `create [name] [flags]` — Create a virtual display. The optional positional `name` sets the device name. Flags supported by `create`:
  - `--width N`, `--w N`        Width in pixels (default 1920)
  - `--height N`, `--h N`       Height in pixels (default 1080)
  - `--refresh N` or `--refresh N.N`  Refresh rate; integer interpreted as milliHz or float as Hz
  - `--hdr`                      Enable HDR
  - `--no-hdr`                   Disable HDR
  - `--primary`                  Make display primary
  - `--adapter <id>`             Adapter LUID (decimal or `0x` hex)
  - `--guid <GUID>`              Supply a GUID to use for the display

- `remove <guid>` — Remove a virtual display by GUID.

- `mode <guid> <w> <h> <refresh> <iso>` — Set mode on a display. `iso` is `1` for isolated layout or `0` for normal.

- `primary <guid>` — Make a virtual display primary.

- `hdr <guid> <0|1>` — Disable (`0`) or enable (`1`) HDR for a display.

- `query <guid>` — Query current mode and HDR state for a display.

- `kill` — Instruct the tray server to exit.

- `sunshine` — Create a display using Sunshine/Apollo environment variables (helper verb).

Examples:

  `SudoVdaController create "My Display" --width=2560 --height=1440 --refresh=119.98`

  `SudoVdaController remove 01234567-89ab-cdef-0123-456789abcdef`


### Tray agent

- Create / Remove virtual displays via the system tray menu.
- Toggle HDR on supported displays.
- Set a display as primary.
- Change display mode (resolution / refresh rate) using dialogs or presets.
- Enable/disable displays (persisted to `ConfigStore`).
- Auto-start and background management.

## Contributing

Contributions are welcome. Please open issues or pull requests on the repository. Follow existing code style and keep changes scoped and tested.

## License

This project is licensed under the MIT License — see the `LICENSE` file in the repository root for details.

