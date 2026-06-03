# Razer Fn-Fix Daemon (`razer-fn-fix`)

A low-level Just-In-Time (JIT) hardware layer daemon that restores missing navigation shortcut functionality (`Home`, `End`, `Print Screen`, `Pause`, `Sleep`) when combining the **Fn** modifier with existing keys on Razer keyboards under Linux.

---

## The Problem
Many modern Razer keyboards (such as the BlackWidow V4 Pro 75%) handle the `Fn` key via internal hardware profiles or proprietary Windows software (Synapse). Under Linux, holding down `Fn` drops the standard matrix link and fails to map navigation macros, leaving users without standard dedicated keys like `Home`, `End`, or `Print Screen`.

## The Solution
`razer-fn-fix` is a secure, lightweight system daemon written in C.

Unlike traditional re-mappers that constantly log your keyboard inputs (creating security risks and latency), this driver runs in a **Passive JIT (Just-In-Time) Interception Layer**:
1. It sits entirely silent, consuming 0% CPU, while monitoring *only* the low-level hardware `hidraw` channel for the physical `Fn` keypress.
2. The instant `Fn` is pressed down, it securely grabs exclusive custody of the keyboard matrix.
3. If you type a designated navigation shortcut, it intercepts it, translates it to the proper key code (`Home`, `End`, etc.), injects it into the kernel virtual user-input stream (`uinput`), and **immediately releases the keyboard**.
4. If you type any other key (e.g., `Fn + A`), it consumes the keypress and closes the Fn latch state.

## NOTE:
It is impossible to implement hypershift functionality on Linux exactly as it is on Windows because while the Fn key is held, the keyboard consumes all other keypresses until it receives a hypershift acknowledgement from the system, which is managed by the Razer Synapse driver. Unless this is reverse engineered and the handshake is implemented, no keypresses are sent while Fn is held down.

This fix uses a sequential latching state machine instead. When Fn is pressed and released, a latch is toggled that grabs the keyboard and consumes the next key. Rather than holding Fn and then pressing your shortcut key, you must press them sequentially instead.

---

## Default Keyboard Mappings

When the `Fn` key is used, the driver intercepts the following matrix codes and morphs them into standard navigation functions:

| Physical Key Combination | Injected System Mapping | Target Functionality |
| :--- | :--- | :--- |
| **`Fn`** + **`PageUp`** | `KEY_HOME` | Moves cursor to start of line |
| **`Fn`** + **`PageDown`** | `KEY_END` | Moves cursor to end of line |
| **`Fn`** + **`P`** | `KEY_SYSRQ` | Print Screen |
| **`Fn`** + **`Delete`** | `KEY_SLEEP` | Triggers System Sleep / Suspend |
| **`Fn`** + **`Insert`** | `KEY_PAUSE` | Pause / Break |

---

## Installation

### Arch Linux (via AUR)
The easiest way to install `razer-fn-fix` on Arch Linux or any Arch-based derivative (like CachyOS, EndeavourOS, or Manjaro) is using an AUR helper such as `yay` or `paru`:

```bash
yay -S razer-fn-fix-git
```

### Manual Compilation (Other Distributions)
Because this daemon depends entirely on native Linux kernel headers (linux/uinput.h), it can be compiled without external libraries on any modern distribution.

#### Clone the repository:

```bash
git clone [https://github.com/Adam-AtlasSoftware/razer-fn-fix.git](https://github.com/Adam-AtlasSoftware/razer-fn-fix.git)
cd razer-fn-fix
```

#### Compile the source:

```bash
gcc -O3 razer_driver.c -o razer_driver
```

#### Deploy the binary and systemd service:

```bash
sudo cp razer_driver /usr/bin/razer_driver
sudo cp razer-fn.service /usr/lib/systemd/system/razer-fn.service
```

## Configuration & Customization
Before launching the daemon, verify your system's hardware event paths inside razer_driver.c. By default, the driver targets standard layouts common to the BlackWidow V4 Pro line:

```C
#define TARGET_EVENT_NODE  "/dev/input/event8"
#define TARGET_HIDRAW_NODE "/dev/hidraw6"
```

If your desktop configuration maps your keyboard matrix or raw HID signals to different hardware channels, look them up using lsinput or evtest, adjust those strings in razer_driver.c, and recompile.

## Execution & Lifecycle Management
Start and enable the background daemon using systemd:

```bash
# Start the driver immediately
sudo systemctl start razer-fn.service

# Enable it to run automatically on system boot
sudo systemctl enable razer-fn.service
```

To monitor driver states or confirm execution stability:

```bash
sudo systemctl status razer-fn.service
```

To stop or disable the driver background processes:

```bash
sudo systemctl stop razer-fn.service
sudo systemctl disable razer-fn.service
```

## License
This project is released under the GPL3 License. See LICENSE for details.
