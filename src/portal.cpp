#include <atomic>
#include "portal.h"
#include "clipboard.h"
#include "libei_handler.h"
#include "wayland_virtual_keyboard.h"
#include "wayland_virtual_pointer.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <algorithm>
#include <sys/mman.h>
#include <fcntl.h>

extern "C" {
#include <libei.h>
#include "libei-1.0/libeis.h"
#include <wayland-client-protocol.h>
}

// ---- Monitor layout (hypr-remote local patch) ----------------------------
// Synergy/Deskflow sends absolute positions within one rectangle. Hyprland
// maps virtual-pointer absolute motion onto the bounding box of all monitors
// without keeping it on a monitor, so the cursor could enter the dead space
// of a non-rectangular layout. We advertise the real bounding box and clamp
// every absolute position onto the nearest monitor.
#include <cstdio>
#include <cmath>
#include <vector>
#include <mutex>

namespace {
struct LayoutRect { double x, y, w, h; };
struct Layout {
    std::vector<LayoutRect> monitors;
    double min_x = 0, min_y = 0, width = 1920, height = 1080;
};

Layout parse_hyprctl_monitors() {
    Layout layout;
    FILE* pipe = popen("hyprctl monitors 2>/dev/null", "re");
    if (!pipe) return layout;
    std::vector<LayoutRect> rects;
    char line[512];
    int pw = 0, ph = 0, px = 0, py = 0, transform = 0;
    double scale = 1.0;
    bool have_mode = false;
    auto flush = [&]() {
        if (!have_mode) return;
        double w = pw / (scale > 0 ? scale : 1.0);
        double h = ph / (scale > 0 ? scale : 1.0);
        if (transform % 2 == 1) std::swap(w, h);
        rects.push_back({double(px), double(py), std::round(w), std::round(h)});
        have_mode = false; scale = 1.0; transform = 0;
    };
    while (fgets(line, sizeof line, pipe)) {
        if (strncmp(line, "Monitor ", 8) == 0) { flush(); continue; }
        double hz; int a, b, c, d;
        if (sscanf(line, " %dx%d@%lf at %dx%d", &a, &b, &hz, &c, &d) == 5) {
            pw = a; ph = b; px = c; py = d; have_mode = true;
        } else if (sscanf(line, " scale: %lf", &hz) == 1) {
            scale = hz;
        } else if (sscanf(line, " transform: %d", &a) == 1) {
            transform = a;
        }
    }
    flush();
    pclose(pipe);
    if (rects.empty()) return layout;
    double x0 = rects[0].x, y0 = rects[0].y, x1 = x0, y1 = y0;
    for (auto& r : rects) {
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
    }
    layout.monitors = rects;
    layout.min_x = x0; layout.min_y = y0;
    layout.width = x1 - x0; layout.height = y1 - y0;
    return layout;
}

std::mutex g_layout_mutex;
Layout g_layout;
// Monitor the cursor is currently on (index into layout.monitors), -1 = unknown.
// Positions that fall in dead space are clamped onto this monitor, so the
// cursor slides along its edge instead of jumping to whichever monitor is nearest.
int g_current_monitor = -1;
Layout current_layout(bool force = false) {
    std::lock_guard<std::mutex> lock(g_layout_mutex);
    if (force || g_layout.monitors.empty()) {
        Layout fresh = parse_hyprctl_monitors();
        if (!fresh.monitors.empty()) g_layout = fresh;
    }
    return g_layout;
}

// x, y are relative to the bounding box origin. If the point is on a monitor it is
// kept as-is; otherwise it is clamped onto the current monitor (or the nearest
// one if the current monitor is unknown).
void clamp_to_monitors(const Layout& layout, double& x, double& y) {
    if (layout.monitors.empty()) return;
    double lx = x + layout.min_x, ly = y + layout.min_y;
    for (size_t i = 0; i < layout.monitors.size(); ++i) {
        auto& r = layout.monitors[i];
        if (lx >= r.x && lx < r.x + r.w && ly >= r.y && ly < r.y + r.h) {
            g_current_monitor = static_cast<int>(i);
            return;
        }
    }
    if (g_current_monitor >= 0 && g_current_monitor < static_cast<int>(layout.monitors.size())) {
        auto& r = layout.monitors[g_current_monitor];
        x = std::clamp(lx, r.x, r.x + r.w - 1) - layout.min_x;
        y = std::clamp(ly, r.y, r.y + r.h - 1) - layout.min_y;
        return;
    }
    double best_x = lx, best_y = ly, best_d = -1;
    for (auto& r : layout.monitors) {
        double cx = std::clamp(lx, r.x, r.x + r.w - 1);
        double cy = std::clamp(ly, r.y, r.y + r.h - 1);
        double d = (cx - lx) * (cx - lx) + (cy - ly) * (cy - ly);
        if (best_d < 0 || d < best_d) { best_d = d; best_x = cx; best_y = cy; g_current_monitor = static_cast<int>(&r - &layout.monitors[0]); }
    }
    x = best_x - layout.min_x;
    y = best_y - layout.min_y;
}

// Turns Synergy's absolute positions into a cursor position that never
// overshoots into dead space. The cursor follows Synergy's *movement*, scaled
// per axis so that the cursor reaches a layout edge exactly when Synergy's
// position reaches the matching edge of its rectangle. Any gap left by pushing
// against a wall therefore shrinks smoothly, and Synergy hitting the edge of its
// rectangle never stops the cursor short of the real screen edge.
struct AbsTracker {
    bool have_prev = false;
    double raw_x = 0, raw_y = 0;   // last position Synergy sent
    double real_x = 0, real_y = 0; // last position we sent to Hyprland
};
AbsTracker g_abs;

double follow_axis(double raw_prev, double raw_new, double real_prev, double extent) {
    double d = raw_new - raw_prev;
    if (d == 0) return real_prev;
    double hi = extent - 1;
    double raw_room = d < 0 ? raw_prev : hi - raw_prev;   // distance to Synergy's bound
    double real_room = d < 0 ? real_prev : hi - real_prev; // distance to layout bound
    double factor = raw_room > 0.5 ? real_room / raw_room : 12.0;
    factor = std::clamp(factor, 0.25, 12.0);
    double out = real_prev + d * factor;
    if ((d < 0 && raw_new <= 0) || (d > 0 && raw_new >= hi)) out = d < 0 ? 0 : hi;
    return std::clamp(out, 0.0, hi);
}

void track_absolute(const Layout& layout, double& x, double& y) {
    double rx = x, ry = y;
    double dx = rx - g_abs.raw_x, dy = ry - g_abs.raw_y;
    if (!g_abs.have_prev || std::abs(dx) > 400 || std::abs(dy) > 400) {
        // Entry point or jump: go where Synergy says, onto the nearest monitor.
        g_current_monitor = -1;
    } else {
        x = follow_axis(g_abs.raw_x, rx, g_abs.real_x, layout.width);
        y = follow_axis(g_abs.raw_y, ry, g_abs.real_y, layout.height);
    }
    clamp_to_monitors(layout, x, y);
    g_abs = {true, rx, ry, x, y};
}
} // namespace

static const char* PORTAL_INTERFACE = "org.freedesktop.impl.portal.RemoteDesktop";
static const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
static const char* CLIPBOARD_INTERFACE = "org.freedesktop.impl.portal.Clipboard";
static const char* SESSION_INTERFACE = "org.freedesktop.impl.portal.Session";
static const char* PORTAL_ERROR_FAILED = "org.freedesktop.portal.Error.Failed";

static void reply_failed(sdbus::MethodCall& call, const std::string& message) {
    call.createErrorReply(sdbus::Error(sdbus::Error::Name{PORTAL_ERROR_FAILED}, message)).send();
}

// Use development name if requested, otherwise use standard name
static const char* PORTAL_NAME = "org.freedesktop.impl.portal.desktop.hypr-remote";

Portal::Portal() : libei_handler(nullptr) {
}

Portal::~Portal() {
    cleanup();
}

bool Portal::init(LibEIHandler* handler, WaylandClipboard* clip) {
    libei_handler = handler;
    clipboard = clip;
    
    try {
        // Create D-Bus connection to SESSION bus (not system bus)
        connection = sdbus::createSessionBusConnection();
        
        // Request the portal name
        connection->requestName(sdbus::ServiceName{PORTAL_NAME});
        
        // Create the portal object
        object = sdbus::createObject(*connection, sdbus::ObjectPath{PORTAL_PATH});
        std::cout << "Portal D-Bus interface registered at " << PORTAL_NAME << std::endl;
        std::cout << "Portal registered on SESSION bus (not system bus)" << std::endl;
        std::cout << "Portal version: 2" << std::endl;
        std::cout << "Portal path: " << PORTAL_PATH << std::endl;
        std::cout << "Portal interface: " << PORTAL_INTERFACE << std::endl;

        // Register RemoteDesktop interface methods with correct signatures
        // Finalize the object
        object->addVTable(
            sdbus::MethodVTableItem{sdbus::MethodName{"CreateSession"}, sdbus::Signature{"oosa{sv}"}, {}, sdbus::Signature{"ua{sv}"}, {}, [this](sdbus::MethodCall call) { CreateSession(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"SelectDevices"}, sdbus::Signature{"oosa{sv}"}, {}, sdbus::Signature{"ua{sv}"}, {}, [this](sdbus::MethodCall call) { SelectDevices(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"Start"}, sdbus::Signature{"oossa{sv}"}, {}, sdbus::Signature{"ua{sv}"}, {}, [this](sdbus::MethodCall call) { Start(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"NotifyPointerMotion"}, sdbus::Signature{"oa{sv}dd"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { NotifyPointerMotion(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"NotifyPointerButton"}, sdbus::Signature{"oa{sv}iu"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { NotifyPointerButton(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"NotifyKeyboardKeycode"}, sdbus::Signature{"oa{sv}iu"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { NotifyKeyboardKeycode(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"NotifyPointerAxis"}, sdbus::Signature{"oa{sv}dd"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { NotifyPointerAxis(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"ConnectToEIS"}, sdbus::Signature{"osa{sv}"}, {}, sdbus::Signature{"h"}, {}, [this](sdbus::MethodCall call) { ConnectToEIS(std::move(call)); }, {}},
            sdbus::PropertyVTableItem{sdbus::PropertyName{"version"}, sdbus::Signature{"u"}, [](sdbus::PropertyGetReply& reply) { reply << (uint32_t)2; }, {}, {}}
        ).forInterface(sdbus::InterfaceName{PORTAL_INTERFACE});

        // Clipboard interface (always exported; methods fail if data-control is missing)
        object->addVTable(
            sdbus::MethodVTableItem{sdbus::MethodName{"RequestClipboard"}, sdbus::Signature{"oa{sv}"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { RequestClipboard(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"SetSelection"}, sdbus::Signature{"oa{sv}"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { SetSelection(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"SelectionWrite"}, sdbus::Signature{"ou"}, {}, sdbus::Signature{"h"}, {}, [this](sdbus::MethodCall call) { SelectionWrite(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"SelectionWriteDone"}, sdbus::Signature{"oub"}, {}, sdbus::Signature{""}, {}, [this](sdbus::MethodCall call) { SelectionWriteDone(std::move(call)); }, {}},
            sdbus::MethodVTableItem{sdbus::MethodName{"SelectionRead"}, sdbus::Signature{"os"}, {}, sdbus::Signature{"h"}, {}, [this](sdbus::MethodCall call) { SelectionRead(std::move(call)); }, {}},
            sdbus::SignalVTableItem{sdbus::SignalName{"SelectionOwnerChanged"}, sdbus::Signature{"oa{sv}"}, {}, {}},
            sdbus::SignalVTableItem{sdbus::SignalName{"SelectionTransfer"}, sdbus::Signature{"osu"}, {}, {}},
            sdbus::PropertyVTableItem{sdbus::PropertyName{"version"}, sdbus::Signature{"u"}, [](sdbus::PropertyGetReply& reply) { reply << (uint32_t)1; }, {}, {}}
        ).forInterface(sdbus::InterfaceName{CLIPBOARD_INTERFACE});
    } catch (const sdbus::Error& e) {
        std::cerr << "Failed to initialize D-Bus portal: " << e.what() << std::endl;
        std::cerr << "This is normal if another portal is already running or if running outside a desktop session." << std::endl;
        cleanup();
        return false;
    }

    // Clipboard failures never stop mouse and keyboard (clipboard.h, items 8-9).
    if (clipboard) {
        ClipboardCallbacks callbacks;
        callbacks.owner_changed = [this](const std::vector<std::string>& mimes) { emit_owner_changed(mimes); };
        callbacks.transfer_requested = [this](const std::string& mime, uint32_t serial) { return emit_transfer(mime, serial); };
        if (!clipboard->init(std::move(callbacks)))
            std::cerr << "clipboard: disabled; remote input still works" << std::endl;
    }
    return true;
}

void Portal::cleanup() {
    // Session objects must go before the connection. The clipboard thread has
    // been stopped by now (main stops it before cleanup()). Destroy them outside
    // sessions_mtx: destroying an object takes the sdbus lock, and method
    // handlers run under that lock and take sessions_mtx (lock order).
    std::map<std::string, SessionState> closing;
    std::vector<std::unique_ptr<sdbus::IObject>> closing_retired;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        closing.swap(sessions);
        closing_retired.swap(retired);
        clip_session.clear();
    }
    closing.clear();
    closing_retired.clear();

    if (object) {
        object.reset();
    }
    
    if (connection) {
        connection.reset();
    }
}

void Portal::run() {
    if (!connection) return;
    
    std::cout << "🔄 Starting proper sdbus D-Bus event loop..." << std::endl;
    std::cout << "📡 Portal ready to receive D-Bus calls!" << std::endl;
    
    try {
        // Use the official sdbus event loop 
        // This will block and properly handle all incoming D-Bus method calls
        connection->enterEventLoop();
    } catch (const sdbus::Error& e) {
        std::cerr << "D-Bus error in portal loop: " << e.what() << std::endl;
    }
    
    std::cout << "🛑 D-Bus event loop stopped" << std::endl;
}

void Portal::stop() {
    // Exit the sdbus event loop cleanly
    if (connection) {
        connection->leaveEventLoop();
    }
}

void Portal::CreateSession(sdbus::MethodCall call) {
    std::cout << "🔥 RemoteDesktop CreateSession called!" << std::endl;
    std::cout << "📋 FLOW: Step 1/4 - CreateSession" << std::endl;
    std::cout << "🎯 This indicates deskflow found our portal!" << std::endl;
    
    // Extract parameters according to D-Bus signature "oosa{sv}"
    sdbus::ObjectPath request_handle;
    sdbus::ObjectPath session_handle;
    std::string app_id;
    std::map<std::string, sdbus::Variant> options;
    
    try {
        call >> request_handle >> session_handle >> app_id >> options;
        
        std::cout << "Request handle: " << request_handle << std::endl;
        std::cout << "Session handle: " << session_handle << std::endl;
        std::cout << "App ID: " << app_id << std::endl;
        std::cout << "Options received:" << std::endl;
        for (const auto& option : options) {
            std::cout << "  " << option.first << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error extracting CreateSession parameters: " << e.what() << std::endl;
        auto reply = call.createReply();
        reply << static_cast<uint32_t>(1); // Error
        reply << std::map<std::string, sdbus::Variant>{};
        reply.send();
        return;
    }
    
    export_session(session_handle);

    // Create session response
    std::map<std::string, sdbus::Variant> response;
    response["session_handle"] = sdbus::Variant(session_handle);
    
    auto reply = call.createReply();
    reply << static_cast<uint32_t>(0); // Success
    reply << response;
    reply.send();
    
    std::cout << "✅ CreateSession completed successfully" << std::endl;
    std::cout << "📋 NEXT: Client should call SelectDevices or Start" << std::endl;
}

void Portal::SelectDevices(sdbus::MethodCall call) {
    std::cout << "🔥 RemoteDesktop SelectDevices called!" << std::endl;
    std::cout << "📋 FLOW: Step 2/4 - SelectDevices" << std::endl;
    
    // Extract parameters according to D-Bus signature "oosa{sv}"
    sdbus::ObjectPath request_handle;
    sdbus::ObjectPath session_handle;
    std::string app_id;
    std::map<std::string, sdbus::Variant> options;
    
    try {
        call >> request_handle >> session_handle >> app_id >> options;
        
        std::cout << "Request handle: " << request_handle << std::endl;
        std::cout << "Session handle: " << session_handle << std::endl;
        std::cout << "App ID: " << app_id << std::endl;
        std::cout << "Options received:" << std::endl;
        for (const auto& option : options) {
            std::cout << "  " << option.first << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error extracting SelectDevices parameters: " << e.what() << std::endl;
        auto reply = call.createReply();
        reply << static_cast<uint32_t>(1); // Error
        reply << std::map<std::string, sdbus::Variant>{};
        reply.send();
        return;
    }
    
    // Response - allow all devices
    std::map<std::string, sdbus::Variant> response;
    response["types"] = sdbus::Variant(static_cast<uint32_t>(7)); // keyboard | pointer | touchscreen
    
    auto reply = call.createReply();
    reply << static_cast<uint32_t>(0); // Success
    reply << response;
    reply.send();
    
    std::cout << "✅ SelectDevices completed for session: " << session_handle << std::endl;
    std::cout << "📋 NEXT: Client should call Start" << std::endl;
}

void Portal::Start(sdbus::MethodCall call) {
    std::cout << "🔥 RemoteDesktop Start called - This is where the magic happens!" << std::endl;
    std::cout << "📋 FLOW: Step 3/4 - Start session" << std::endl;
    std::cout << "🎯 If you see this, deskflow is following the portal flow correctly!" << std::endl;

    // Extract parameters according to D-Bus signature "oossa{sv}"
    sdbus::ObjectPath request_handle;
    sdbus::ObjectPath session_handle;
    std::string app_id;
    std::string parent_window;
    std::map<std::string, sdbus::Variant> options;
    
    try {
        call >> request_handle >> session_handle >> app_id >> parent_window >> options;
        
        std::cout << "Request handle: " << request_handle << std::endl;
        std::cout << "Session handle: " << session_handle << std::endl;
        std::cout << "App ID: " << app_id << std::endl;
        std::cout << "Parent window: " << parent_window << std::endl;
        std::cout << "Options received:" << std::endl;
        for (const auto& option : options) {
            std::cout << "  " << option.first << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error extracting Start parameters: " << e.what() << std::endl;
        auto reply = call.createReply();
        reply << static_cast<uint32_t>(1); // Error
        reply << std::map<std::string, sdbus::Variant>{};
        reply.send();
        return;
    }
    
    // Check if we have a working LibEI handler
    if (!libei_handler) {
        std::cerr << "No LibEI handler available for remote session" << std::endl;
        auto reply = call.createReply();
        reply << static_cast<uint32_t>(1); // Error
        reply << std::map<std::string, sdbus::Variant>{};
        reply.send();
        return;
    }
    
    std::cout << "✅ Using existing LibEI handler for input processing" << std::endl;
    
    // Start the remote desktop session
    std::map<std::string, sdbus::Variant> response;
    response["devices"] = sdbus::Variant(static_cast<uint32_t>(7)); // keyboard | pointer | touchscreen

    // Ask the clipboard before taking sessions_mtx (see portal.h).
    bool clipboard_enabled = clipboard && clipboard->available();
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        auto& state = sessions[session_handle];
        clipboard_enabled = clipboard_enabled && state.clipboard_requested;
    }
    if (clipboard_enabled) response["clipboard_enabled"] = sdbus::Variant(true);
    std::cout << "Start: clipboard_enabled=" << (clipboard_enabled ? "true" : "false") << std::endl;
    
    auto reply = call.createReply();
    reply << static_cast<uint32_t>(0); // Success
    reply << response;
    reply.send();

    if (clipboard_enabled) {
        std::string previous;
        {
            std::lock_guard<std::mutex> lock(sessions_mtx);
            previous = clip_session;
            clip_session = session_handle;
        }
        if (!previous.empty() && previous != std::string(session_handle)) {
            std::cout << "clipboard: session " << previous << " replaced by "
                      << session_handle << std::endl;
            clipboard->session_ended();  // before announce(): commands run in FIFO order
        }
        clipboard->announce();
    }
    
    std::cout << "Start completed - remote desktop session active for session: " << session_handle << std::endl;
    std::cout << "LibEI handler ready for input processing" << std::endl;
    std::cout << "📋 NEXT: Client should now call ConnectToEIS for modern input" << std::endl;
}

void Portal::NotifyPointerMotion(sdbus::MethodCall call) {
    
    // Extract parameters
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    double dx, dy;
    call >> session_handle >> options >> dx >> dy;
    
    
    // Get current time for wayland events
    uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Forward to virtual pointer via libei handler's virtual pointer
    std::unique_lock<std::mutex> input_lock;
    if (libei_handler) input_lock = std::unique_lock<std::mutex>(libei_handler->input_mutex);
    if (libei_handler && libei_handler->pointer) {
        libei_handler->pointer->send_motion(time, dx, dy);
        libei_handler->pointer->send_frame();
    }
    
    if (input_lock.owns_lock()) input_lock.unlock();
    auto reply = call.createReply();
    reply.send();
}

void Portal::NotifyPointerButton(sdbus::MethodCall call) {
    
    // Extract parameters
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    int32_t button;
    uint32_t state;
    call >> session_handle >> options >> button >> state;
    
    
    // Get current time for wayland events
    uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Forward to virtual pointer
    std::unique_lock<std::mutex> input_lock;
    if (libei_handler) input_lock = std::unique_lock<std::mutex>(libei_handler->input_mutex);
    if (libei_handler && libei_handler->pointer) {
        libei_handler->pointer->send_button(time, static_cast<uint32_t>(button), state);
        libei_handler->pointer->send_frame();
    }
    
    if (input_lock.owns_lock()) input_lock.unlock();
    auto reply = call.createReply();
    reply.send();
}

void Portal::NotifyKeyboardKeycode(sdbus::MethodCall call) {
    
    // Extract parameters
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    int32_t keycode;
    uint32_t state;
    call >> session_handle >> options >> keycode >> state;
    
    
    // Get current time for wayland events
    uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Forward to virtual keyboard
    std::unique_lock<std::mutex> input_lock;
    if (libei_handler) input_lock = std::unique_lock<std::mutex>(libei_handler->input_mutex);
    if (libei_handler && libei_handler->keyboard) {
        libei_handler->keyboard->send_key(time, static_cast<uint32_t>(keycode), state);
    }
    
    if (input_lock.owns_lock()) input_lock.unlock();
    auto reply = call.createReply();
    reply.send();
}

void Portal::NotifyPointerAxis(sdbus::MethodCall call) {
    
    // Extract parameters
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    double dx, dy;
    call >> session_handle >> options >> dx >> dy;
    
    
    // Get current time for wayland events
    uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Forward to virtual pointer with proper Wayland scroll protocol
    std::unique_lock<std::mutex> input_lock;
    if (libei_handler) input_lock = std::unique_lock<std::mutex>(libei_handler->input_mutex);
    if (libei_handler && libei_handler->pointer) {
        // Set axis source for legacy scroll events
        libei_handler->pointer->send_axis_source(WL_POINTER_AXIS_SOURCE_WHEEL);
        
        // Send scroll events for both axes if non-zero
        if (dx != 0.0) {
            libei_handler->pointer->send_axis(time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
            libei_handler->pointer->send_axis_stop(time, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
        }
        if (dy != 0.0) {
            libei_handler->pointer->send_axis(time, WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
            libei_handler->pointer->send_axis_stop(time, WL_POINTER_AXIS_VERTICAL_SCROLL);
        }
        libei_handler->pointer->send_frame();
    }
    
    if (input_lock.owns_lock()) input_lock.unlock();
    auto reply = call.createReply();
    reply.send();
}

void Portal::ConnectToEIS(sdbus::MethodCall call) {
    std::cout << "🔥 RemoteDesktop ConnectToEIS called!" << std::endl;
    std::cout << "📋 FLOW: Step 5/5 - Connect to EIS (Modern approach!)" << std::endl;
    std::cout << "🎯 THIS IS THE KEY METHOD! Deskflow uses this for input!" << std::endl;
    
    // Extract parameters according to D-Bus signature "osa{sv}"
    sdbus::ObjectPath session_handle;
    std::string app_id;
    std::map<std::string, sdbus::Variant> options;
    
    try {
        call >> session_handle >> app_id >> options;
        
        std::cout << "Session handle: " << session_handle << std::endl;
        std::cout << "App ID: " << app_id << std::endl;
        std::cout << "Options received:" << std::endl;
        for (const auto& option : options) {
            std::cout << "  " << option.first << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error extracting ConnectToEIS parameters: " << e.what() << std::endl;
        call.createErrorReply(sdbus::Error(sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, "Failed to extract parameters")).send();
        return;
    }
    
    if (!libei_handler || !libei_handler->keyboard || !libei_handler->pointer) {
        std::cerr << "Virtual devices not available" << std::endl;
        call.createErrorReply(sdbus::Error(sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, "Virtual devices not available")).send();
        return;
    }
    
    // Create a socket pair - one end for deskflow, one end for our EIS server
    int socket_pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_pair) == -1) {
        std::cerr << "Error creating socket pair: " << strerror(errno) << std::endl;
        call.createErrorReply(sdbus::Error(sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, "Failed to create socket pair")).send();
        return;
    }
    
    int client_fd = socket_pair[0];  // This goes to deskflow
    int server_fd = socket_pair[1];  // This stays with us for EIS server
    
    std::cout << "✅ Created socket pair - client_fd: " << client_fd << ", server_fd: " << server_fd << std::endl;
    
    // Start a thread to run a proper EIS server
    std::thread([this, server_fd]() {
        std::cout << "📡 Starting proper EIS server thread..." << std::endl;
        
        // Create EIS server context (similar to hyprland-eis)
        struct eis* eis_context = eis_new(nullptr);
        if (!eis_context) {
            std::cerr << "Failed to create EIS server context" << std::endl;
            close(server_fd);
            return;
        }
        
        // Set up logging (optional, for debugging)
        // eis_log_set_priority(eis_context, EIS_LOG_PRIORITY_DEBUG);
        
        std::cout << "✅ EIS server context created" << std::endl;
        
        // Create a temporary socket and immediately connect our FD to it
        // This is a workaround since libeis may not support direct FD setup
        
        // Alternative approach: Create a proper EIS server socket and handle the connection
        char socket_path[256];
        static std::atomic<unsigned> eis_session_counter{0};
        snprintf(socket_path, sizeof(socket_path), "/tmp/hypr-portal-eis-%d-%u", getpid(), eis_session_counter++);
        
        // Setup EIS backend with temporary socket
        int rc = eis_setup_backend_socket(eis_context, socket_path);
        if (rc != 0) {
            std::cerr << "Failed to setup EIS backend socket: " << strerror(errno) << std::endl;
            eis_unref(eis_context);
            close(server_fd);
            return;
        }
        
        std::cout << "✅ EIS backend socket created at: " << socket_path << std::endl;
        
        // Now we need to bridge between our socket_pair and the EIS socket
        // Start a bridge thread to forward data between them
        std::thread bridge_thread([server_fd, socket_path]() {
            std::cout << "🌉 Starting socket bridge..." << std::endl;
            
            // Connect to the EIS socket
            int eis_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (eis_sock == -1) {
                std::cerr << "Failed to create bridge socket" << std::endl;
                return;
            }
            
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
            
            // Wait a bit for the EIS server to start
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            if (connect(eis_sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
                std::cerr << "Failed to connect to EIS socket: " << strerror(errno) << std::endl;
                close(eis_sock);
                return;
            }
            
            std::cout << "✅ Bridge connected to EIS socket" << std::endl;
            
            // Bridge data bidirectionally between server_fd and eis_sock
            fd_set read_fds;
            char buffer[4096];
            
            // Make sockets non-blocking for better performance
            int flags = fcntl(server_fd, F_GETFL, 0);
            fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
            flags = fcntl(eis_sock, F_GETFL, 0);
            fcntl(eis_sock, F_SETFL, flags | O_NONBLOCK);
            
            while (true) {
                FD_ZERO(&read_fds);
                FD_SET(server_fd, &read_fds);
                FD_SET(eis_sock, &read_fds);
                
                int max_fd = std::max(server_fd, eis_sock);
                struct timeval timeout = {0, 100000}; // 100ms timeout for better responsiveness
                
                int activity = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
                
                if (activity < 0) {
                    std::cerr << "Bridge select error: " << strerror(errno) << std::endl;
                    break;
                }
                
                if (activity == 0) continue; // timeout
                
                // Forward data from deskflow (server_fd) to EIS server (eis_sock)
                if (FD_ISSET(server_fd, &read_fds)) {
                    ssize_t bytes = read(server_fd, buffer, sizeof(buffer));
                    if (bytes <= 0) {
                        if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            std::cout << "Deskflow disconnected from bridge" << std::endl;
                            break;
                        }
                    } else {
                        if (write(eis_sock, buffer, bytes) != bytes) {
                            std::cerr << "Failed to forward data to EIS server" << std::endl;
                            break;
                        }
                        //std::cout << "🔄 Forwarded " << bytes << " bytes from deskflow to EIS" << std::endl;
                    }
                }
                
                // Forward data from EIS server (eis_sock) to deskflow (server_fd)
                if (FD_ISSET(eis_sock, &read_fds)) {
                    ssize_t bytes = read(eis_sock, buffer, sizeof(buffer));
                    if (bytes <= 0) {
                        if (bytes == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                            std::cout << "EIS server disconnected from bridge" << std::endl;
                            break;
                        }
                    } else {
                        if (write(server_fd, buffer, bytes) != bytes) {
                            std::cerr << "Failed to forward data to deskflow" << std::endl;
                            break;
                        }
                        //std::cout << "🔄 Forwarded " << bytes << " bytes from EIS to deskflow" << std::endl;
                    }
                }
            }
            
            std::cout << "🌉 Socket bridge stopped" << std::endl;
            close(eis_sock);
        });
        bridge_thread.detach();
        
        // Run the EIS server event loop (adapted from hyprland-eis)
        std::cout << "🚀 Starting EIS server event loop..." << std::endl;
        
        bool stop = false;
        struct pollfd fds = {
            .fd = eis_get_fd(eis_context),
            .events = POLLIN,
            .revents = 0,
        };
        
        while (!stop) {
            int nevents = poll(&fds, 1, 100); // Reduced timeout to 100ms for better responsiveness
            if (nevents == -1) {
                std::cerr << "EIS poll error: " << strerror(errno) << std::endl;
                break;
            }
            
            if (nevents == 0) continue; // timeout
            
            // Process all pending EIS events in one go - this is crucial for scroll
            eis_dispatch(eis_context);
            
            // Handle ALL events in the queue immediately
            struct eis_event* event;
            while ((event = eis_get_event(eis_context)) != nullptr) {
                
                // Handle EIS events and forward to virtual devices
                bool client_gone = eis_event_get_type(event) == EIS_EVENT_CLIENT_DISCONNECT;
                handle_eis_event(event);
                eis_event_unref(event);
                if (client_gone) stop = true;
            }
        }
        
        std::cout << "📡 EIS server thread stopped" << std::endl;
        unlink(socket_path); // Clean up socket file
        eis_unref(eis_context);
        close(server_fd);
    }).detach();
    
    // Return the client file descriptor to deskflow
    auto reply = call.createReply();
    
    // Create a unix file descriptor object and pass it to the client
    sdbus::UnixFd unix_fd{client_fd};
    reply << unix_fd;
    reply.send();
    
    std::cout << "✅ ConnectToEIS completed - socket fd sent to deskflow" << std::endl;
    std::cout << "🎯 Deskflow can now send EIS events through fd " << client_fd << std::endl;
    std::cout << "📡 Proper EIS server thread is running with socket bridge" << std::endl;
}

void Portal::handle_eis_event(struct eis_event* event) {
    std::unique_lock<std::mutex> input_lock;
    if (libei_handler) input_lock = std::unique_lock<std::mutex>(libei_handler->input_mutex);
    enum eis_event_type type = eis_event_get_type(event);
    
    // Log all events for debugging
    const char* event_name = "UNKNOWN";
    switch (type) {
        case EIS_EVENT_CLIENT_CONNECT: event_name = "CLIENT_CONNECT"; break;
        case EIS_EVENT_CLIENT_DISCONNECT: event_name = "CLIENT_DISCONNECT"; break;
        case EIS_EVENT_SEAT_BIND: event_name = "SEAT_BIND"; break;
        case EIS_EVENT_DEVICE_START_EMULATING: event_name = "DEVICE_START_EMULATING"; break;
        case EIS_EVENT_DEVICE_STOP_EMULATING: event_name = "DEVICE_STOP_EMULATING"; break;
        case EIS_EVENT_POINTER_MOTION: event_name = "POINTER_MOTION"; break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE: event_name = "POINTER_MOTION_ABSOLUTE"; break;
        case EIS_EVENT_BUTTON_BUTTON: event_name = "BUTTON_BUTTON"; break;
        case EIS_EVENT_SCROLL_DELTA: event_name = "SCROLL_DELTA"; break;
        case EIS_EVENT_SCROLL_DISCRETE: event_name = "SCROLL_DISCRETE"; break;
        case EIS_EVENT_KEYBOARD_KEY: event_name = "KEYBOARD_KEY"; break;
        case EIS_EVENT_FRAME: event_name = "FRAME"; break;
        default: event_name = "UNKNOWN"; break;
    }
    //std::cout << "🔥 EIS EVENT: " << event_name << " (type=" << type << ")" << std::endl;
    
    switch (type) {
        case EIS_EVENT_CLIENT_CONNECT: {
            struct eis_client* client = eis_event_get_client(event);
            std::cout << "🔌 EIS: Client connected: " << eis_client_get_name(client) << std::endl;
            
            // Accept the client connection
            eis_client_connect(client);
            
            // Add a seat for this client (required for devices)
            struct eis_seat* seat = eis_client_new_seat(client, "hyprland-portal-seat");
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
            eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
            eis_seat_add(seat);
            
            std::cout << "💺 EIS: Seat added for client with capabilities" << std::endl;
            break;
        }
        
        case EIS_EVENT_CLIENT_DISCONNECT:
            std::cout << "🔌 EIS: Client disconnected" << std::endl;
            break;
            
        case EIS_EVENT_SEAT_BIND: {
            struct eis_seat* seat = eis_event_get_seat(event);
            std::cout << "💺 EIS: Seat bound by client" << std::endl;
            
            // Add pointer device
            struct eis_device* pointer = eis_seat_new_device(seat);
            eis_device_configure_name(pointer, "Hyprland Portal Pointer");
            eis_device_configure_capability(pointer, EIS_DEVICE_CAP_POINTER);
            eis_device_configure_capability(pointer, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_device_configure_capability(pointer, EIS_DEVICE_CAP_BUTTON);
            eis_device_configure_capability(pointer, EIS_DEVICE_CAP_SCROLL);
            
            // Set pointer region (screen size)
            struct eis_region* region = eis_device_new_region(pointer);
            {
                Layout layout = current_layout(true);
                eis_region_set_size(region, static_cast<uint32_t>(layout.width), static_cast<uint32_t>(layout.height));
                std::cout << "EIS: Pointer region " << layout.width << "x" << layout.height
                          << " across " << layout.monitors.size() << " monitor(s)" << std::endl;
            }
            eis_region_add(region);
            
            eis_device_add(pointer);
            eis_device_resume(pointer);
            
            // Add keyboard device with proper keymap setup
            struct eis_device* keyboard = eis_seat_new_device(seat);
            eis_device_configure_name(keyboard, "Hyprland Portal Keyboard");
            eis_device_configure_capability(keyboard, EIS_DEVICE_CAP_KEYBOARD);
            
            // Set up a basic keymap for proper modifier key handling
            // This is crucial for key combinations like Meta+Enter to work
            const char* keymap_str = 
                "xkb_keymap {\n"
                "xkb_keycodes  { include \"evdev+aliases(qwerty)\" };\n"
                "xkb_types     { include \"complete\" };\n"
                "xkb_compat    { include \"complete\" };\n"
                "xkb_symbols   { include \"pc+us+inet(evdev)\" };\n"
                "xkb_geometry  { include \"pc(pc105)\" };\n"
                "};\n";
                
            // Create a memory file for the keymap
            size_t keymap_size = strlen(keymap_str);
            int memfd = memfd_create("keymap", MFD_CLOEXEC);
            if (memfd >= 0) {
                if (write(memfd, keymap_str, keymap_size) == (ssize_t)keymap_size) {
                    struct eis_keymap* keymap = eis_device_new_keymap(keyboard, 
                        EIS_KEYMAP_TYPE_XKB, memfd, keymap_size);
                    if (keymap) {
                        eis_keymap_add(keymap);
                        std::cout << "🗝️ EIS: Keymap configured for proper modifier handling" << std::endl;
                    }
                }
                close(memfd);
            }
            
            eis_device_add(keyboard);
            eis_device_resume(keyboard);
            
            std::cout << "🖱️ EIS: Pointer and keyboard devices added with enhanced features" << std::endl;
            break;
        }
        
        case EIS_EVENT_DEVICE_START_EMULATING: {
            struct eis_device* device = eis_event_get_device(event);
            std::cout << "🎮 EIS: Device started emulating: " << eis_device_get_name(device) << std::endl;
            g_current_monitor = -1; // cursor (re)entered this screen
            // Pick up monitor changes here, not during motion. (The pointer region
            // size is still fixed at SEAT_BIND; that limitation is unchanged.)
            current_layout(true);
            g_abs.have_prev = false;
            break;
        }
        
        case EIS_EVENT_DEVICE_STOP_EMULATING: {
            struct eis_device* device = eis_event_get_device(event);
            std::cout << "🎮 EIS: Device stopped emulating: " << eis_device_get_name(device) << std::endl;
            break;
        }
        
        case EIS_EVENT_POINTER_MOTION: {
            double dx = eis_event_pointer_get_dx(event);
            double dy = eis_event_pointer_get_dy(event);
            
            
            // Forward to virtual pointer
            if (libei_handler && libei_handler->pointer) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                libei_handler->pointer->send_motion(time, dx, dy);
                libei_handler->pointer->send_frame();
            }
            break;
        }
        
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE: {
            double x = eis_event_pointer_get_absolute_x(event);
            double y = eis_event_pointer_get_absolute_y(event);
            
            
            // Forward to virtual pointer  
            if (libei_handler && libei_handler->pointer) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                Layout layout = current_layout();
                track_absolute(layout, x, y);
                libei_handler->pointer->send_motion_absolute(time,
                    static_cast<uint32_t>(std::lround(x)), static_cast<uint32_t>(std::lround(y)),
                    static_cast<uint32_t>(layout.width), static_cast<uint32_t>(layout.height));
                libei_handler->pointer->send_frame();
            }
            break;
        }
        
        case EIS_EVENT_BUTTON_BUTTON: {
            uint32_t button = eis_event_button_get_button(event);
            bool is_press = eis_event_button_get_is_press(event);
            
            
            // Forward to virtual pointer
            if (libei_handler && libei_handler->pointer) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                libei_handler->pointer->send_button(time, button, is_press ? 1 : 0);
                libei_handler->pointer->send_frame();
            }
            break;
        }
        
        case EIS_EVENT_SCROLL_DELTA: {
            double dx = eis_event_scroll_get_dx(event);
            double dy = eis_event_scroll_get_dy(event);
            
            
            // Debug: Check if we have the required components
            
            // Forward to virtual pointer with proper Wayland scroll protocol
            if (libei_handler && libei_handler->pointer) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                
                
                // Set axis source - wheel is the most common source for EIS scroll events
                libei_handler->pointer->send_axis_source(WL_POINTER_AXIS_SOURCE_WHEEL);
                    
                // Scale the scroll values appropriately for Wayland
                double scale_factor = 15.0; // Good default for smooth scrolling
                
                if (dx != 0.0) {
                    libei_handler->pointer->send_axis(time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx * scale_factor);
                    // Send axis stop to complete the scroll event
                    libei_handler->pointer->send_axis_stop(time, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
                }
                if (dy != 0.0) {
                    libei_handler->pointer->send_axis(time, WL_POINTER_AXIS_VERTICAL_SCROLL, dy * scale_factor);
                    // Send axis stop to complete the scroll event  
                    libei_handler->pointer->send_axis_stop(time, WL_POINTER_AXIS_VERTICAL_SCROLL);
                }
                libei_handler->pointer->send_frame();
            }
            break;
        }
        
        case EIS_EVENT_SCROLL_DISCRETE: {
            int32_t dx = eis_event_scroll_get_discrete_dx(event);
            int32_t dy = eis_event_scroll_get_discrete_dy(event);
            
            if (dx == 0 && dy == 0) {
                break;
                // Assume this is a vertical scroll event and give it a default value
                //dy = -1; // Negative = scroll up (standard)
                //std::cout << "🔄 Discrete values are 0, assuming vertical scroll step: dy=" << dy << std::endl;
            }

            
            // If discrete values are 0, assume vertical scroll with 1 step (common case)
                        
            // Forward discrete scroll if we have actual values (now that we fixed 0,0 case)
            if (libei_handler && libei_handler->pointer && (dx != 0 || dy != 0)) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                    
                // Set axis source for discrete scroll (wheel clicks)
                libei_handler->pointer->send_axis_source(WL_POINTER_AXIS_SOURCE_WHEEL);
                
                // Use standard scroll value per click - simple and effective
                double scroll_value = 15.0;
                int axis = dx == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
                libei_handler->pointer->send_axis_discrete(time, dx, dy);
                // libei_handler->pointer->send_axis_stop(time, axis);
            
                libei_handler->pointer->send_frame();
            }
            break;
        }
        
        case EIS_EVENT_KEYBOARD_KEY: {
            uint32_t keycode = eis_event_keyboard_get_key(event);
            bool is_press = eis_event_keyboard_get_key_is_press(event);
            
            
            // Debug: Check if we have the required components
            
            // Forward to virtual keyboard with immediate modifier updates
            if (libei_handler && libei_handler->keyboard) {
                uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                    
                
                // Update modifier state BEFORE sending the key event (using raw keycode)
                update_modifier_state(keycode, is_press);
                
                
                // Send modifier state first - this is crucial for key combinations like Meta+Enter
                libei_handler->keyboard->send_modifiers(modifier_state_depressed, 
                                                      modifier_state_latched,
                                                      modifier_state_locked, 
                                                      modifier_state_group);
                
                // Send the actual key event with the raw keycode (no conversion needed!)
                libei_handler->keyboard->send_key(time, keycode, is_press ? 1 : 0);
                
                // Send modifiers again after the key event to ensure state consistency
                libei_handler->keyboard->send_modifiers(modifier_state_depressed, 
                                                      modifier_state_latched,
                                                      modifier_state_locked, 
                                                      modifier_state_group);
                                                      
            }
            break;
        }
        
        case EIS_EVENT_FRAME:
            // Frame events group related events together; nothing to do
            break;
            
        default:
            std::cout << "❓ EIS: Unhandled event type: " << type << std::endl;
            break;
    }
}

void Portal::update_modifier_state(uint32_t keycode, bool is_press) {
    // EIS uses raw Linux input keycodes (NOT XKB keycodes with +8 offset)
    // These are the standard Linux input event keycodes
    bool is_modifier = false;
    uint32_t modifier_mask = 0;
    
    switch (keycode) {
        case 42:  // Shift_L (raw keycode 42)
        case 54:  // Shift_R (raw keycode 54)
            is_modifier = true;
            modifier_mask = MOD_SHIFT;
            break;
            
        case 29:  // Control_L (raw keycode 29)
        case 97:  // Control_R (raw keycode 97)
            is_modifier = true;
            modifier_mask = MOD_CTRL;
            break;
            
        case 56:  // Alt_L (raw keycode 56)
        case 100: // Alt_R (raw keycode 100)
            is_modifier = true;
            modifier_mask = MOD_ALT;
            break;
            
        case 125: // Super_L (raw keycode 125) - Meta/Windows key
        case 126: // Super_R (raw keycode 126)
            is_modifier = true;
            modifier_mask = MOD_META;
            break;
            
        case 58:  // Caps_Lock (raw keycode 58)
            // Caps lock is special - toggle on press only
            if (is_press) {
                modifier_state_locked ^= MOD_CAPS; // Toggle caps lock state
            }
            return;
            
        case 69:  // Num_Lock (raw keycode 69)
            // Num lock is special - toggle on press only
            if (is_press) {
                modifier_state_locked ^= MOD_NUM; // Toggle num lock state
            }
            return;
    }
    
    if (is_modifier) {
        if (is_press) {
            modifier_state_depressed |= modifier_mask;
        } else {
            modifier_state_depressed &= ~modifier_mask;
        }
    }
}

// ---- org.freedesktop.impl.portal.Session ------------------------------------

void Portal::export_session(const sdbus::ObjectPath& session_handle) {
    const std::string handle = session_handle;
    // Objects are created and destroyed outside sessions_mtx.
    std::unique_ptr<sdbus::IObject> old;
    std::vector<std::unique_ptr<sdbus::IObject>> closed;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        closed.swap(retired);
        auto it = sessions.find(handle);
        if (it != sessions.end()) old = std::move(it->second.obj);
    }
    old.reset();
    closed.clear();

    try {
        auto obj = sdbus::createObject(*connection, session_handle);
        obj->addVTable(
            sdbus::MethodVTableItem{sdbus::MethodName{"Close"}, sdbus::Signature{""}, {}, sdbus::Signature{""}, {}, [this, handle](sdbus::MethodCall call) { SessionClose(std::move(call), handle); }, {}},
            sdbus::SignalVTableItem{sdbus::SignalName{"Closed"}, sdbus::Signature{""}, {}, {}},
            sdbus::PropertyVTableItem{sdbus::PropertyName{"version"}, sdbus::Signature{"u"}, [](sdbus::PropertyGetReply& reply) { reply << (uint32_t)1; }, {}, {}}
        ).forInterface(sdbus::InterfaceName{SESSION_INTERFACE});
        std::lock_guard<std::mutex> lock(sessions_mtx);
        sessions[handle].obj = std::move(obj);
    } catch (const sdbus::Error& e) {
        std::cerr << "Failed to export Session object at " << handle << ": " << e.what() << std::endl;
    }
}

void Portal::SessionClose(sdbus::MethodCall call, const std::string& session_handle) {
    call.createReply().send();

    bool was_clipboard_session = false;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        auto it = sessions.find(session_handle);
        if (it != sessions.end()) {
            // Can't destroy an object inside its own callback: retire it.
            if (it->second.obj) retired.push_back(std::move(it->second.obj));
            sessions.erase(it);
        }
        if (clip_session == session_handle) {
            clip_session.clear();
            was_clipboard_session = true;
        }
    }
    std::cout << "Session closed: " << session_handle << std::endl;
    if (was_clipboard_session && clipboard) clipboard->session_ended();
}

// ---- org.freedesktop.impl.portal.Clipboard ----------------------------------

bool Portal::is_clip_session(const std::string& session_handle) {
    if (!clipboard || !clipboard->available()) return false;
    std::lock_guard<std::mutex> lock(sessions_mtx);
    return clip_session == session_handle;
}

void Portal::RequestClipboard(sdbus::MethodCall call) {
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    try {
        call >> session_handle >> options;
    } catch (const sdbus::Error&) {
        reply_failed(call, "Invalid arguments");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        sessions[session_handle].clipboard_requested = true;
    }
    std::cout << "clipboard requested for session " << session_handle << std::endl;
    call.createReply().send();
}

void Portal::SetSelection(sdbus::MethodCall call) {
    sdbus::ObjectPath session_handle;
    std::map<std::string, sdbus::Variant> options;
    try {
        call >> session_handle >> options;
    } catch (const sdbus::Error&) {
        reply_failed(call, "Invalid arguments");
        return;
    }
    if (!is_clip_session(session_handle)) {
        reply_failed(call, "Clipboard not enabled for this session");
        return;
    }
    std::vector<std::string> mimes;
    auto it = options.find("mime_types");
    if (it != options.end()) {
        try {
            mimes = it->second.get<std::vector<std::string>>();
        } catch (const sdbus::Error&) {
            reply_failed(call, "mime_types must be of type as");
            return;
        }
    }
    mimes.erase(std::remove(mimes.begin(), mimes.end(), std::string(kOwnedMarker)), mimes.end());
    std::cout << "clipboard: SetSelection with " << mimes.size() << " type(s)" << std::endl;
    if (!mimes.empty()) clipboard->claim(std::move(mimes));
    call.createReply().send();
}

void Portal::SelectionWrite(sdbus::MethodCall call) {
    sdbus::ObjectPath session_handle;
    uint32_t serial = 0;
    try {
        call >> session_handle >> serial;
    } catch (const sdbus::Error&) {
        reply_failed(call, "Invalid arguments");
        return;
    }
    if (!is_clip_session(session_handle)) {
        reply_failed(call, "Clipboard not enabled for this session");
        return;
    }
    int fd = clipboard->take_write_fd(serial);
    if (fd < 0) {
        reply_failed(call, "Unknown or expired serial");
        return;
    }
    // adopt_fd: the UnixFd owns our copy and closes it after the reply (Types.h:329).
    sdbus::UnixFd unix_fd{fd, sdbus::adopt_fd};
    auto reply = call.createReply();
    reply << unix_fd;
    reply.send();
}

void Portal::SelectionWriteDone(sdbus::MethodCall call) {
    sdbus::ObjectPath session_handle;
    uint32_t serial = 0;
    bool success = false;
    try {
        call >> session_handle >> serial >> success;
    } catch (const sdbus::Error&) {
        reply_failed(call, "Invalid arguments");
        return;
    }
    if (!is_clip_session(session_handle)) {
        reply_failed(call, "Clipboard not enabled for this session");
        return;
    }
    clipboard->write_done(serial, success);
    call.createReply().send();
}

void Portal::SelectionRead(sdbus::MethodCall call) {
    sdbus::ObjectPath session_handle;
    std::string mime_type;
    try {
        call >> session_handle >> mime_type;
    } catch (const sdbus::Error&) {
        reply_failed(call, "Invalid arguments");
        return;
    }
    if (!is_clip_session(session_handle)) {
        reply_failed(call, "Clipboard not enabled for this session");
        return;
    }
    int fd = clipboard->open_read(mime_type);
    if (fd < 0) {
        reply_failed(call, "MIME type not offered by the current selection");
        return;
    }
    sdbus::UnixFd unix_fd{fd, sdbus::adopt_fd};
    auto reply = call.createReply();
    reply << unix_fd;
    reply.send();
}

// Called on the Wayland thread with no WaylandClipboard lock held. Emitting is
// thread-safe (IObject.h:57-59); we never emit while holding sessions_mtx.
void Portal::emit_owner_changed(const std::vector<std::string>& mimes) {
    std::string session;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        session = clip_session;
    }
    if (session.empty() || !object) return;
    try {
        std::map<std::string, sdbus::Variant> options;
        options["mime_types"] = sdbus::Variant(mimes);
        options["session_is_owner"] = sdbus::Variant(false);
        auto signal = object->createSignal(sdbus::InterfaceName{CLIPBOARD_INTERFACE},
                                           sdbus::SignalName{"SelectionOwnerChanged"});
        signal << sdbus::ObjectPath{session} << options;
        object->emitSignal(signal);
    } catch (const sdbus::Error& e) {
        std::cerr << "clipboard: SelectionOwnerChanged failed: " << e.what() << std::endl;
    }
}

bool Portal::emit_transfer(const std::string& mime, uint32_t serial) {
    std::string session;
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        session = clip_session;
    }
    if (session.empty() || !object) return false;
    try {
        auto signal = object->createSignal(sdbus::InterfaceName{CLIPBOARD_INTERFACE},
                                           sdbus::SignalName{"SelectionTransfer"});
        signal << sdbus::ObjectPath{session} << mime << serial;
        object->emitSignal(signal);
        return true;
    } catch (const sdbus::Error& e) {
        std::cerr << "clipboard: SelectionTransfer failed: " << e.what() << std::endl;
        return false;
    }
}
