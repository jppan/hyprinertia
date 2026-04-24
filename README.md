# hyprinertia

Pointer inertia for Hyprland.

`hyprinertia` is a small Hyprland plugin that keeps pointer motion moving briefly after input stops. It is mainly tuned for touchpads, but it can be enabled for mice or specific pointer devices through Hyprland config.

## Features

- Adds smooth pointer inertia after real pointer motion ends.
- Defaults to touchpad-only behavior.
- Supports per-device filtering with `touchpad`, `mouse`, `all`, `*`, or exact Hyprland/libinput device names.
- Exposes friction, multiplier, velocity limits, deadzone, tick rate, and idle delay as Hyprland config values.
- Stops cleanly when disabled or unloaded.

## Requirements

- Hyprland headers and plugin API for the running Hyprland build
- CMake 3.27 or newer
- A C++23 compiler
- `pkg-config`
- Development packages for Hyprland, libdrm, libinput, libudev, pangocairo, pixman, wayland-server, and xkbcommon

## Build

```sh
make
```

The build writes `hyprinertia.so` in the project root.

## Install With hyprpm

This repo includes `hyprpm.toml`, so it can be used as a Hyprland plugin repository:

```sh
hyprpm add <repo-url>
hyprpm enable hyprinertia
```

Reload Hyprland after enabling the plugin if needed.

## Manual Load

After building, load the plugin directly from Hyprland:

```sh
hyprctl plugin load ./hyprinertia.so
```

Use an absolute path if loading it from your Hyprland config or from another working directory.

## Configuration

All options live under `plugin:hyprinertia`.

```ini
plugin {
    hyprinertia {
        enabled = true
        devices = touchpad
        friction = 0.88
        multiplier = 0.70
        deadzone = 0.0
        min_velocity = 0.05
        max_velocity = 45.0
        tick_ms = 8
        idle_start_ms = 18
    }
}
```

## Options

| Option | Default | Description |
| --- | ---: | --- |
| `enabled` | `true` | Enables or disables inertia. |
| `devices` | `touchpad` | Comma-separated device filter. Accepts `touchpad`, `mouse`, `all`, `*`, or exact device names. |
| `friction` | `0.88` | Velocity retained on each tick. Lower values stop faster. |
| `multiplier` | `0.70` | Scales the captured pointer velocity before inertia starts. |
| `deadzone` | `0.0` | Minimum real motion magnitude required before inertia arms. |
| `min_velocity` | `0.05` | Velocity threshold where inertia stops. |
| `max_velocity` | `45.0` | Maximum stored inertia velocity. Set to `0` to avoid clamping. |
| `tick_ms` | `8` | Timer interval for synthetic inertia motion. |
| `idle_start_ms` | `18` | Delay after the last real pointer motion before inertia begins. |

## Notes

Hyprland plugins are tied to the exact Hyprland build they were compiled against. If Hyprland updates, rebuild the plugin before loading it again.
