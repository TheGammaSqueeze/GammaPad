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
