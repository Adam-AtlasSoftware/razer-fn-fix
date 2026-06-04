# Razer Fn-Fix Daemon (`razer-fn-fix`)

System daemon that maps navigation shortcuts (`Home`, `End`, `Print Screen`, `Pause`, `Sleep`) when combining the `Fn` modifier with other keys on Razer keyboards under Linux.

## Problem Description
Many Razer keyboards handle the `Fn` key internally. Under Linux, holding `Fn` prevents standard navigation macros from registering, disabling shortcuts like `Fn` + `PageUp` for `Home`.

## Solution & Architecture
`razer-fn-fix` is a background daemon written in C that intercepts inputs to inject custom keycodes:
1. It monitors `hidraw` or `evdev` nodes for designated modifier sequences.
2. Upon modifier activation, it grabs custody of the input device.
3. If a mapped shortcut sequence is typed, it translates the event, injects the new keycode into `/dev/uinput`, and releases the device.
4. If a non-mapped key is typed or the timeout is reached, it flushes the input buffer and releases the device.

## Sequential Latching
Because the hardware does not always stream concurrent inputs while `Fn` is held, a sequential latching state machine is supported. Pressing and releasing the modifier opens a latch for a configured duration (`latch_timeout_ms`). Shortcuts can be pressed sequentially during this window.

## Installation

### Arch Linux
Install from the AUR:
```bash
yay -S razer-fn-fix-git
```
Add user to the input group:
```bash
sudo usermod -aG razer-input $USER
```

### Manual Compilation
Requirements: GCC and Linux kernel headers (`linux/uinput.h`).

```bash
git clone https://github.com/Adam-AtlasSoftware/razer-fn-fix.git
cd razer-fn-fix
gcc -O3 razer_driver.c cJSON.c -lm -o razer_driver
sudo cp razer_driver /usr/bin/razer_driver
sudo cp razer-fn.service /usr/lib/systemd/system/razer-fn.service
sudo cp 99-razer-input.rules /usr/lib/udev/rules.d/99-razer-input.rules
```

## Configuration

The daemon parses config files at `/etc/razer-fn/config.json`. It monitors this directory via `inotify` and hot-reloads configuration automatically when changes are written.

### Schema Example (`/etc/razer-fn/config.json`)
```json
{
  "profiles": [
    {
      "id": "blackwidow_v4_pro_75_wired",
      "user_name": "My Main BlackWidow 75%",
      "hardware_match": "PRODUCT=3/1532/02a6",
      "enabled": true,
      "latch_timeout_ms": 1500,
      "modifiers": [
        {
          "name": "Fn",
          "type": "hidraw",
          "activation_sequence": ["0x04", "0x01"],
          "release_sequence": ["0x04", "0x00", "0x05", "0x51", "0x01"]
        }
      ],
      "macros": [
        {
          "modifier": "Fn",
          "sequence": ["KEY_PAGEUP"],
          "output": "KEY_HOME",
          "action_type": "remap"
        }
      ]
    }
  ]
}
```

### Default Fallback
If no config file is present, the daemon uses a built-in fallback mapping for standard Razer devices:
- `Fn` + `PageUp` -> `KEY_HOME`
- `Fn` + `PageDown` -> `KEY_END`
- `Fn` + `P` -> `KEY_SYSRQ` (Print Screen)
- `Fn` + `Delete` -> `KEY_SLEEP`
- `Fn` + `Insert` -> `KEY_PAUSE`

## Service Management
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now razer-fn.service
```

## License
GPL3.
