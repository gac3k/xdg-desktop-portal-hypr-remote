#pragma once

extern "C" {
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
}

class WaylandVirtualPointer {
public:
    WaylandVirtualPointer();
    ~WaylandVirtualPointer();
    
    bool init();
    void cleanup();
    
    // Pointer input methods
    void send_motion(uint32_t time, double dx, double dy);
    void send_motion_absolute(uint32_t time, uint32_t x, uint32_t y, uint32_t x_extent, uint32_t y_extent);
    void send_button(uint32_t time, uint32_t button, uint32_t state);
    void send_axis(uint32_t time, uint32_t axis, double value);
    void send_axis_source(uint32_t axis_source);
    void send_axis_discrete(uint32_t time, int32_t discrete_dx, int32_t discrete_dy);
    void send_axis_stop(uint32_t time, uint32_t axis);
    void send_frame();

    // Registry callback functions (must be public)
    static void registry_global(void* data, struct wl_registry* registry,
                              uint32_t name, const char* interface, uint32_t version);
    static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name);
    
private:
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct zwlr_virtual_pointer_manager_v1* pointer_manager;
    struct zwlr_virtual_pointer_v1* virtual_pointer;
}; 