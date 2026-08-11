# xdg-desktop-portal-hypr-remote

An implementation of the `org.freedesktop.impl.portal.RemoteDesktop` portal for
Hyprland (and other wlroots-based compositors), built on libei and the Wayland
virtual input protocols.

Hyprland has no RemoteDesktop backend of its own: `xdg-desktop-portal-hyprland`
implements ScreenCast but not RemoteDesktop, and the only backend that does
(`xdg-desktop-portal-kde`) needs KWin. Applications that ask for remote input
therefore fail with:

```
No such interface "org.freedesktop.portal.RemoteDesktop" on object at path
/org/freedesktop/portal/desktop
```

This portal fills that gap, so clients such as KDE Connect's virtual touchpad or
Deskflow can drive the pointer and keyboard.

## Requirements

- Hyprland (or another compositor implementing `wlr-virtual-pointer-unstable-v1`
  and `virtual-keyboard-unstable-v1`)
- `libei` 1.6 or newer (provides both `libei-1.0` and `libeis-1.0`)
- `sdbus-c++` 2.x
- `wayland`, `wayland-protocols`, `xdg-desktop-portal`
- `cmake`, `pkg-config`, a C++17 compiler

On Arch Linux:

```bash
sudo pacman -S --needed cmake pkgconf gcc libei sdbus-cpp wayland wayland-protocols xdg-desktop-portal
```

## Build and install

```bash
cmake -B build -S . -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

This installs four files, all of which are needed:

| Path | Purpose |
| --- | --- |
| `/usr/bin/xdg-desktop-portal-hypr-remote` | the portal itself |
| `/usr/share/xdg-desktop-portal/portals/hypr-remote.portal` | tells xdg-desktop-portal which interface this backend provides |
| `/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.hypr-remote.service` | lets D-Bus start the portal on demand |
| `/usr/lib/systemd/user/xdg-desktop-portal-hypr-remote.service` | systemd unit used by the D-Bus activation |

Then restart the portal frontend:

```bash
systemctl --user restart xdg-desktop-portal.service
```

That is all: `hypr-remote.portal` declares `UseIn=hyprland`, so
xdg-desktop-portal routes `RemoteDesktop` here on its own.

## Configuration (only if you already have a portals.conf)

A `portals.conf` takes precedence over the `UseIn` key, so if you have your own
`~/.config/xdg-desktop-portal/hyprland-portals.conf` — a common thing to do to
force the GTK file picker, for instance — its `default=` line also covers
`RemoteDesktop`, and requests will fail because neither `hyprland` nor `gtk`
implements it. Add an explicit line for this backend:

```ini
[preferred]
default=hyprland;gtk
org.freedesktop.impl.portal.RemoteDesktop=hypr-remote
```

The file name must match your desktop, lowercased — with
`XDG_CURRENT_DESKTOP=Hyprland` it is `hyprland-portals.conf`.

Note that `UseIn` is deprecated in xdg-desktop-portal. It still works and is
what keeps installation configuration-free today, but should it ever be dropped,
the snippet above becomes mandatory. The system-wide default that ships with
Hyprland (`/usr/share/xdg-desktop-portal/hyprland-portals.conf`) cannot be
extended by this package, since that file belongs to the `hyprland` package.

## Verifying it works

The interface should now exist:

```bash
busctl --user introspect org.freedesktop.portal.Desktop \
    /org/freedesktop/portal/desktop | grep RemoteDesktop
```

The backend is activated on demand, so it is normal for it not to appear in the
process list until a client asks for a session. To start it manually and inspect
its methods:

```bash
busctl --user introspect org.freedesktop.impl.portal.desktop.hypr-remote \
    /org/freedesktop/portal/desktop \
    org.freedesktop.impl.portal.RemoteDesktop
```

There is also a standalone check that the compositor accepts virtual input. It
moves the cursor in a circle and clicks:

```bash
./build/test-virtual-input
```

Portal activity is logged to the journal:

```bash
journalctl --user -f | grep hypr-remote
```

A healthy session logs `CreateSession` → `SelectDevices` → `Start` →
`ConnectToEIS`, ending with `EIS fd <n> sent to client`.

## Troubleshooting

**`The name is not activatable`** — D-Bus has not picked up the service file. It
caches the list of activatable names at startup, so after installing run:

```bash
busctl --user call org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus ReloadConfig
```

**`No such interface "org.freedesktop.portal.RemoteDesktop"`** — the portal
config is not being applied. Check that the file is named after your desktop and
that `XDG_CURRENT_DESKTOP` matches.

**`Marshalling failed: Invalid object path passed in arguments`** — the client is
sending input events with an empty session handle, which means session setup
failed earlier. Look further up in the log for the real error.

## Project layout

```
src/
├── main.cpp                          entry point
├── portal.cpp/.h                     D-Bus portal + EIS server
├── libei_handler.cpp/.h              libei context and virtual devices
├── wayland_virtual_keyboard.cpp/.h   virtual-keyboard-unstable-v1
└── wayland_virtual_pointer.cpp/.h    wlr-virtual-pointer-unstable-v1
protocols/                            Wayland protocol XML
data/hypr-remote.portal               portal descriptor
```

## License

MIT — see LICENSE.
