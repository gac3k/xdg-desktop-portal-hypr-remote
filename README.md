# Hyprland Remote Desktop Portal

A complete implementation of the `org.freedesktop.impl.portal.RemoteDesktop` portal for Hyprland, providing remote desktop capabilities using libei and Wayland virtual input protocols.

It also implements `org.freedesktop.impl.portal.Clipboard` (text and `image/png`) through `ext-data-control-v1`, so clipboard sharing works with clients such as Synergy and Deskflow.

#### **System Installation**

```bash
# Configure, build and install (the install step also installs data/hypr-remote.portal)
cmake -B build -DDEVELOPMENT_MODE=OFF -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBEXECDIR=lib
cmake --build build
sudo cmake --install build
```

The service files expect the binary in `libexecdir` (`/usr/lib/xdg-desktop-portal-hypr-remote`), while `cmake --install` puts it in `bindir`; packagers should move it. The install step does not install the service files; copy the generated ones yourself:

- D-Bus service: `build/org.freedesktop.impl.portal.desktop.hypr-remote.service` to `/usr/share/dbus-1/services/`
- systemd user unit: `build/contrib/systemd/xdg-desktop-portal-hypr-remote.service` to `/usr/lib/systemd/user/`

Then route the interfaces to this portal in `hyprland-portals.conf` (for example `~/.config/xdg-desktop-portal/hyprland-portals.conf`):

```ini
[preferred]
org.freedesktop.impl.portal.RemoteDesktop=hypr-remote
org.freedesktop.impl.portal.Clipboard=hypr-remote
```

and restart the portal:

```bash
systemctl --user restart xdg-desktop-portal
```

**Security note:** the portal approves remote-input and clipboard requests without a prompt, but only from host apps (empty `app_id`), such as Synergy run as a systemd user service. Sandboxed apps (Flatpak, Snap) are refused unless their app ID is listed in `HYPR_REMOTE_ALLOWED_APP_IDS` (comma-separated; an empty item means host apps; `*` allows all). Set it with `systemctl --user edit xdg-desktop-portal-hypr-remote`, adding `[Service]` / `Environment=HYPR_REMOTE_ALLOWED_APP_IDS=,org.example.App`. Any unsandboxed process on your session bus can still call the portal directly.

## 🚀 Quick Start

### Manual Build

Install dependencies:
- cmake, pkg-config, gcc
- wayland-client, wayland-protocols, wayland-scanner
- libei-1.0 (libei and libeis), sdbus-c++ 2.x, systemd

Then:
```bash
./build.sh
```
## 🔧 Troubleshooting

### ✅ "Permission denied" D-Bus Errors - SOLVED
- **Fixed**: Now uses session bus instead of system bus
- **Fixed**: Correct method signatures prevent registration errors

### ✅ "File exists" D-Bus Errors - EXPECTED
- **Normal**: Occurs when another portal uses the same service name
- **Solution**: Use development mode (automatic in `test_portal.sh`)

### Testing Portal Integration

Check if the portal is discoverable:

```bash
# List all portal services
busctl --user list | grep portal

# Monitor D-Bus calls to your portal
busctl --user monitor org.freedesktop.impl.portal.desktop.hypr-remote

# Test method calls
busctl --user call org.freedesktop.impl.portal.desktop.hypr-remote \
  /org/freedesktop/portal/desktop \
  org.freedesktop.impl.portal.RemoteDesktop \
  CreateSession 'a{sv}' 0
```
## 📁 Project Structure

```
xdg-desktop-portal-hypr-remote/
├── src/
│   ├── main.cpp                    # Main application entry point
│   ├── portal.cpp/.h               # D-Bus RemoteDesktop implementation, EIS handling
│   ├── clipboard.cpp/.h            # Clipboard implementation (ext-data-control-v1)
│   ├── wayland_virtual_keyboard.cpp/.h  # Virtual keyboard protocol
│   ├── wayland_virtual_pointer.cpp/.h   # Virtual pointer protocol
│   └── libei_handler.cpp/.h        # libei/libeis event processing
├── protocols/
│   ├── ext-data-control-v1.xml               # Clipboard protocol
│   ├── virtual-keyboard-unstable-v1.xml      # Wayland keyboard protocol
│   └── wlr-virtual-pointer-unstable-v1.xml   # wlroots pointer protocol
├── data/
│   └── hypr-remote.portal                     # Portal interface declaration
├── contrib/systemd/
│   └── xdg-desktop-portal-hypr-remote.service.in  # systemd user unit template
├── org.freedesktop.impl.portal.desktop.hypr-remote.service.in  # D-Bus service template
├── shell.nix                       # NixOS development environment
├── CMakeLists.txt                  # Build configuration
├── build.sh                        # Build script
├── test_portal.sh                  # Development testing script
└── README.md                       # This file
```

## 🧪 Testing Commands

```bash
# Build and test
./test_portal.sh

# Manual testing
nix-shell
./build.sh
./build/xdg-desktop-portal-hypr-remote

# D-Bus testing
busctl --user introspect org.freedesktop.impl.portal.desktop.hypr-remote /org/freedesktop/portal/desktop
busctl --user call org.freedesktop.impl.portal.desktop.hypr-remote /org/freedesktop/portal/desktop org.freedesktop.impl.portal.RemoteDesktop CreateSession 'a{sv}' 0
```

## 🤝 Contributing

1. Use the provided `shell.nix` for development
2. Follow the existing code structure
3. Test with `./test_portal.sh`
4. Ensure keyboard, pointer and clipboard all work
5. Verify D-Bus method signatures match the portal specification

## 📄 License

MIT License.
