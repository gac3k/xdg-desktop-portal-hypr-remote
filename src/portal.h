#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "libei-1.0/libeis.h"
}

class LibEIHandler;
class WaylandClipboard;

class Portal {
public:
    Portal();
    ~Portal();
    
    // Registers both vtables, then calls clipboard->init() (see clipboard.h, item 9).
    bool init(LibEIHandler* handler, WaylandClipboard* clipboard);
    void cleanup();
    void run();
    void stop();
    
private:
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> object;
    LibEIHandler* libei_handler;
    WaylandClipboard* clipboard = nullptr;
    bool running;

    // Session table (clipboard.h, item 3). Never call into `clipboard` or emit a
    // signal while holding sessions_mtx.
    struct SessionState {
        bool clipboard_requested = false;
        bool started = false;
        std::unique_ptr<sdbus::IObject> obj;   // org.freedesktop.impl.portal.Session
    };
    std::mutex sessions_mtx;
    std::map<std::string, SessionState> sessions;
    std::vector<std::unique_ptr<sdbus::IObject>> retired;  // closed Session objects
    std::string clip_session;                              // "" = none
    
    // Modifier state tracking for proper key combination handling
    uint32_t modifier_state_depressed = 0;
    uint32_t modifier_state_latched = 0;
    uint32_t modifier_state_locked = 0;
    uint32_t modifier_state_group = 0;
    
    // XKB modifier masks for common modifiers
    static constexpr uint32_t MOD_SHIFT = 1 << 0;
    static constexpr uint32_t MOD_CAPS = 1 << 1;
    static constexpr uint32_t MOD_CTRL = 1 << 2;
    static constexpr uint32_t MOD_ALT = 1 << 3;
    static constexpr uint32_t MOD_NUM = 1 << 4;
    static constexpr uint32_t MOD_META = 1 << 6; // Super/Windows key
    
    void update_modifier_state(uint32_t keycode, bool is_press);
    
    // D-Bus method handlers
    void CreateSession(sdbus::MethodCall call);
    void SelectSources(sdbus::MethodCall call);
    void SelectDevices(sdbus::MethodCall call);
    void Start(sdbus::MethodCall call);
    
    // Input notification methods - these are called by remote clients to send input events
    void NotifyPointerMotion(sdbus::MethodCall call);
    void NotifyPointerButton(sdbus::MethodCall call);
    void NotifyKeyboardKeycode(sdbus::MethodCall call);
    void NotifyPointerAxis(sdbus::MethodCall call);
    
    // Modern EIS (Emulated Input Server) method
    void ConnectToEIS(sdbus::MethodCall call);
    
    // org.freedesktop.impl.portal.Clipboard
    void RequestClipboard(sdbus::MethodCall call);
    void SetSelection(sdbus::MethodCall call);
    void SelectionWrite(sdbus::MethodCall call);
    void SelectionWriteDone(sdbus::MethodCall call);
    void SelectionRead(sdbus::MethodCall call);
    bool is_clip_session(const std::string& session_handle);
    void emit_owner_changed(const std::vector<std::string>& mimes);
    bool emit_transfer(const std::string& mime, uint32_t serial);

    // org.freedesktop.impl.portal.Session at each session_handle
    void export_session(const sdbus::ObjectPath& session_handle);
    void SessionClose(sdbus::MethodCall call, const std::string& session_handle);

    // EIS event handling
    void handle_eis_event(struct eis_event* event);
};  