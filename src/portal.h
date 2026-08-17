#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <memory>

extern "C" {
#include "libei-1.0/libeis.h"
}

class LibEIHandler;

class Portal {
public:
    Portal();
    ~Portal();
    
    bool init(LibEIHandler* handler);
    void cleanup();
    void run();
    void stop();
    
private:
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> object;
    LibEIHandler* libei_handler;
    bool running;
    
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
    // These use sdbus-c++ 2.x async-style typed signatures: a Result<...> first
    // arg carries the reply context, followed by the actual D-Bus arguments.
    // The framework infers the input/output signature from the argument list.
    void CreateSession(sdbus::Result<uint32_t, std::map<std::string, sdbus::Variant>> result,
                       sdbus::ObjectPath request_handle,
                       sdbus::ObjectPath session_handle,
                       std::string app_id,
                       std::map<std::string, sdbus::Variant> options);
    void SelectDevices(sdbus::Result<uint32_t, std::map<std::string, sdbus::Variant>> result,
                       sdbus::ObjectPath request_handle,
                       sdbus::ObjectPath session_handle,
                       std::string app_id,
                       std::map<std::string, sdbus::Variant> options);
    void Start(sdbus::Result<uint32_t, std::map<std::string, sdbus::Variant>> result,
               sdbus::ObjectPath request_handle,
               sdbus::ObjectPath session_handle,
               std::string app_id,
               std::string parent_window,
               std::map<std::string, sdbus::Variant> options);

    // Input notification methods - these are called by remote clients to send input events
    void NotifyPointerMotion(sdbus::Result<> result,
                             sdbus::ObjectPath session_handle,
                             std::map<std::string, sdbus::Variant> options,
                             double dx, double dy);
    void NotifyPointerButton(sdbus::Result<> result,
                             sdbus::ObjectPath session_handle,
                             std::map<std::string, sdbus::Variant> options,
                             int32_t button, uint32_t state);
    void NotifyKeyboardKeycode(sdbus::Result<> result,
                                sdbus::ObjectPath session_handle,
                                std::map<std::string, sdbus::Variant> options,
                                int32_t keycode, uint32_t state);
    void NotifyPointerAxis(sdbus::Result<> result,
                           sdbus::ObjectPath session_handle,
                           std::map<std::string, sdbus::Variant> options,
                           double dx, double dy);

    // Modern EIS (Emulated Input Server) method
    void ConnectToEIS(sdbus::Result<sdbus::UnixFd> result,
                      sdbus::ObjectPath session_handle,
                      std::string app_id,
                      std::map<std::string, sdbus::Variant> options);
    
    // EIS event handling
    void handle_eis_event(struct eis_event* event);
};  