# GammaPad
GammaPad is an evolving userspace helper and virtual device layer designed to unify controller (joypad) input, force feedback, and extended features (like mouse mode, LED control, hotplug support, and more) on Android or other Linux-based systems. It aims to make controller input fully configurable, while keeping code well-structured and portable across different distros and devices.

Overview
- Name Change: The project is now formally named GammaPad.
- Controller Input with Epoll: We have switched to an epoll-based loop for monitoring input devices and the virtual uinput device. This ensures minimal latency and an event-driven architecture.
Android Props for Configuration: GammaPad relies on Android property-based configuration (where available) to define custom button mappings, device names, vendor/product IDs, and other behavior. This allows flexible, per-device or per-user overrides without hardcoding.
- Dynamic Device Handling: We avoid using fixed controller names or event IDs. Instead, the system scans and captures relevant event nodes, meaning it can adapt to multiple devices (e.g., AyaNeo controllers, third-party USB or Bluetooth devices) that appear or disappear over time.
- Portable to Other Distros: The code attempts to remain portable, and can be compiled for a variety of Linux-based distributions beyond Android, as the epoll + uinput approach is fairly standard.
- Mouse Mode (GammaOS Core-style): We integrate a second virtual device for mouse functionality, enabling toggling or mixing gamepad + pointer input.
- Custom Definitions via Props: In addition to button mappings, we allow specifying custom device names, vendor/product IDs, LED behaviors, etc. all from properties or external configurations.
- Force Feedback Support: GammaPad provides force feedback (FF) via uinput for rumble effects and advanced FF types. The logic includes:
- Rumble bridging (strong/weak motor).
  - Handling various effect types (e.g., FF_CONSTANT, FF_RAMP, FF_PERIODIC) where possible.
    A background thread to handle toggling or timed-output for the vibrator.
- Hotplug / Reconnect Handling: We must support scenarios where controllers disconnect and reconnect (e.g., on AyaNeo). This includes:
  - Re-grabbing the physical device upon reconnection.
    Preserving or recreating the virtual device state if needed.
- Global Shortcuts via Props: Plans exist to let users define global shortcuts (e.g., for “pause all,” or “launch shell script on Button X pressed for 3s”). This can also tie into external actions or system-level triggers.
- Shell Script Execution: Certain button combos or inputs can trigger external shell scripts or system commands, especially helpful for system tasks (e.g., toggling performance modes, launching apps, etc.).
- Readability & Maintainability: A key requirement: “MAKE CODE READABLE FOR THE LOVE OF GOD.” We have restructured the code into multiple files (e.g., gammapad_main.c, gammapad_controller.c, gammapad_ff.c, gammapad_capture.c, etc.), aiming to keep each part logically separated and documented.
- Init Scripts for Services: The plan is to let init or service scripts launch GammaPad, optionally reading property-based config or hooking into distro-based init systems. This ensures a consistent device environment at boot.
- Joypad Calibration: Future expansions include user-driven calibration, so different controllers or triggers can have consistent ranges or zero points.
- LED Control & FF: Tying LED states to force-feedback or events (e.g., color changes on certain button combos or rumble).
- Fan Control as External Service: We intend not to clutter GammaPad with device-specific fan logic. Instead, an external script or service can be triggered from the events, allowing a separate “fan manager” process.
- Virtual Screen Mapping Support: Possibly remap or intercept certain inputs that could manipulate an on-screen UI, or automatically route them to another subsystem for accessibility or overlay usage.

------

# Current Status

- Core Input Management:
  - Epoll-based loop for capturing physical devices and forwarding events to the virtual gamepad and optional mouse.
  - Works on Android

- Force Feedback (Rumble) Implementation:
  - Supports rumble via uinput.
  - Different effect types partially implemented (FF_RUMBLE, FF_CONSTANT, etc.).
  - Force feedback threads handle toggling the motor or writing to timed-output paths.

- Android Props & Custom Mappings (In Progress):
  - Basic property-based configuration approach exists, but more advanced usage (like global shortcuts, LED color changes) is still under development.

- Hotplug & Reconnect:
  - Preliminary logic to close and re-open devices on disconnection, though extended testing is needed on various hardware (AyaNeo, GPD Win, etc.).

- Mouse Mode:
  - Virtual mouse device creation is functional.
  - Plans for additional toggles or advanced pointer gestures remain in progress.

- Extensibility:
  - Code is modular: gammapad_main.c (entry + epoll), gammapad_controller.c (uinput creation), gammapad_ff.c (force feedback logic), gammapad_capture.c (physical device capture), etc.
  - Intended to let developers or advanced users tweak or add new features without fully rewriting.

- Readability & Documentation:
  - We have consolidated some large blocks of code and documented the major flows.
  - Ongoing improvements are happening to ensure maintainers can easily see how each module interacts.

------

# Boot Crash Fix (spi_joytick kernel driver workaround)

## Problem
GammaPad was crashing at boot with error:
```
sysfs: cannot create duplicate filename '/devices/platform/singleadc-joypad/poll_interval'
singleadc-joypad: create sysfs group fail, error: -17
probe of singleadc-joypad failed with error -17
```

## Root Cause
The `spi_joytick` kernel driver has a bug where it doesn't properly clean up sysfs entries (`poll_interval`, etc.) when the device is unbound. When GammaPad called `bindPrimaryDriver()` after unbind, the kernel's `joypad_probe()` function would fail with `-EEXIST` because the sysfs attributes already existed.

The old code made this worse by attempting bind 3 times, triggering the error repeatedly.

## Solution (in gammapad_capture.c)

Added smart bind/unbind logic:

1. **New function `isDeviceBound()`**: Checks if device is currently bound by looking for `<driverPath>/<deviceName>` in sysfs.

2. **Modified `unbindPrimaryDriver()`**:
   - Only unbinds if device is actually bound
   - Single attempt instead of 3x
   - Increased delay from 1s to 2s for kernel cleanup

3. **Modified `bindPrimaryDriver()`**:
   - Checks if device is already bound before attempting bind
   - If already bound, skips bind entirely (avoids the kernel bug)
   - Single attempt instead of 3x
   - Verifies bind succeeded

4. **Modified `unbindAndRebind()`**: Uses same smart logic for exit cleanup.

## Why This Works
At boot, the device is already bound by the system. The fix detects this and skips the unnecessary rebind, avoiding the kernel driver bug entirely.

------

# Dual Controller Support (Mantis Compatibility)

## Problem
Some apps (like Mantis Gamepad Pro) don't recognize controllers that have `INPUT_PROP_DIRECT` set or have force feedback enabled. However, other programs require the controller to appear as a Bluetooth Xbox controller with FF support.

## Solution
GammaPad now creates **two virtual controllers**:

1. **Main Controller** - Full-featured Xbox Bluetooth controller
   - Bus: `BUS_BLUETOOTH` (0x05)
   - VID: `0x045e` (Microsoft)
   - PID: `0x02fd` (Xbox Wireless Controller)
   - Has `INPUT_PROP_DIRECT`, `INPUT_PROP_BUTTONPAD`, `INPUT_PROP_TOPBUTTONPAD`
   - Full force feedback support
   - Used by most games and programs expecting an Xbox controller

2. **Mantis Controller** - Simplified controller for Mantis compatibility
   - Bus: `BUS_USB` (0x03)
   - VID: `0x045e` (Microsoft)
   - PID: `0x02fe` (different from main)
   - No `INPUT_PROP_DIRECT`
   - No force feedback
   - Name has "(Mantis)" suffix
   - Used by Mantis and similar apps that need a simpler controller profile

## Why Different Identifiers?
The Mantis controller uses `BUS_USB` and a different product ID (`0x02fe`) to prevent programs from confusing the two controllers. If both had identical VID/PID/bus, programs looking for a Bluetooth Xbox controller might pick up the Mantis one instead, causing compatibility issues.

## Events
Both controllers receive the same input events, so either can be used depending on which one the application detects.

## Mantis Toggle (Duplicate Input Prevention)

### Problem
When both virtual controllers are active and an application (or the system) sees both, key presses can be registered twice - once from each controller.

### Solution
A runtime toggle `g_mantisEnabled` controls whether events are forwarded to the Mantis controller.

### Runtime Command
Send commands to GammaPad's stdin to toggle the Mantis controller:

```bash
# Disable Mantis controller (single controller mode - prevents duplicate inputs)
echo "mantis off" | ...

# Enable Mantis controller (dual controller mode)
echo "mantis on" | ...
```

### System Property (Android)
On GammaOS/Android, GammaPad polls the system property `persist.gammaos.mantis` every 500ms:

- `persist.gammaos.mantis=on` - Mantis controller enabled (default)
- `persist.gammaos.mantis=off` - Mantis controller disabled

This allows the Quick Settings tile or other system components to control the Mantis controller without sending commands directly to GammaPad's stdin.

### Quick Settings Integration
On GammaOS, a "Mantis" Quick Settings tile is available that toggles this property. When toggled:
- **Mantis On**: Both controllers receive events (for Mantis Gamepad Pro compatibility)
- **Mantis Off**: Only the main controller receives events (prevents duplicate inputs)

### Default Behavior
By default, `g_mantisEnabled = 1`, so both controllers receive events. This maintains backward compatibility with existing setups that rely on the Mantis controller.
