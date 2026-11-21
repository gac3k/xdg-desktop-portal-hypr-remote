#include "wayland_virtual_pointer.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

// wl_fixed_from_double casts round(d * 256) to int32: NaN/inf/out of range is UB.
static wl_fixed_t to_fixed(double v) {
    if (!std::isfinite(v)) return 0;
    return wl_fixed_from_double(std::clamp(v, -8388607.0, 8388607.0));
}

static const struct wl_registry_listener registry_listener = {
    .global = WaylandVirtualPointer::registry_global,
    .global_remove = WaylandVirtualPointer::registry_global_remove,
};

WaylandVirtualPointer::WaylandVirtualPointer()
    : display(nullptr), registry(nullptr), seat(nullptr), 
      pointer_manager(nullptr), virtual_pointer(nullptr) {
}

WaylandVirtualPointer::~WaylandVirtualPointer() {
    cleanup();
}

bool WaylandVirtualPointer::init() {
    display = wl_display_connect(nullptr);
    if (!display) {
        std::cerr << "Failed to connect to Wayland display" << std::endl;
        return false;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        std::cerr << "Failed to get Wayland registry" << std::endl;
        cleanup();
        return false;
    }

    wl_registry_add_listener(registry, &registry_listener, this);
    wl_display_roundtrip(display);

    if (!pointer_manager) {
        std::cerr << "Compositor does not support wlr-virtual-pointer protocol" << std::endl;
        cleanup();
        return false;
    }

    // Create virtual pointer
    virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
    if (!virtual_pointer) {
        std::cerr << "Failed to create virtual pointer" << std::endl;
        cleanup();
        return false;
    }

    wl_display_roundtrip(display);
    std::cout << "Wayland Virtual Pointer initialized successfully" << std::endl;
    return true;
}

void WaylandVirtualPointer::cleanup() {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_destroy(virtual_pointer);
        virtual_pointer = nullptr;
    }
    if (pointer_manager) {
        zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
        pointer_manager = nullptr;
    }
    if (seat) {
        wl_seat_destroy(seat);
        seat = nullptr;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = nullptr;
    }
    if (display) {
        wl_display_disconnect(display);
        display = nullptr;
    }
}

void WaylandVirtualPointer::registry_global(void* data, struct wl_registry* registry,
                                           uint32_t name, const char* interface, uint32_t version) {
    WaylandVirtualPointer* self = static_cast<WaylandVirtualPointer*>(data);
    
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        self->pointer_manager = static_cast<struct zwlr_virtual_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 
                           std::min(version, 2u)));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        self->seat = static_cast<struct wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
}

void WaylandVirtualPointer::registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
    // Handle global removal if needed
}

void WaylandVirtualPointer::send_motion(uint32_t time, double dx, double dy) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_motion(virtual_pointer, time, 
                                     to_fixed(dx), 
                                     to_fixed(dy));
    }
}

void WaylandVirtualPointer::send_motion_absolute(uint32_t time, uint32_t x, uint32_t y, 
                                               uint32_t x_extent, uint32_t y_extent) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_motion_absolute(virtual_pointer, time, x, y, x_extent, y_extent);
    }
}

void WaylandVirtualPointer::send_button(uint32_t time, uint32_t button, uint32_t state) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_button(virtual_pointer, time, button, state);
    }
}

void WaylandVirtualPointer::send_axis(uint32_t time, uint32_t axis, double value) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_axis(virtual_pointer, time, axis, to_fixed(value));
    }
}

void WaylandVirtualPointer::send_axis_source(uint32_t axis_source) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_axis_source(virtual_pointer, axis_source);
    }
}

void WaylandVirtualPointer::send_axis_discrete(uint32_t time, int32_t dx, int32_t dy) {
    if (virtual_pointer) {
        if(dy < 0) {
            zwlr_virtual_pointer_v1_axis_discrete(virtual_pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-15), -1);
        } else if(dy > 0) {
            zwlr_virtual_pointer_v1_axis_discrete(virtual_pointer, time, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(15), 1);
        }
    }
}

void WaylandVirtualPointer::send_axis_stop(uint32_t time, uint32_t axis) {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_axis_stop(virtual_pointer, time, axis);
    }
}

void WaylandVirtualPointer::send_frame() {
    if (virtual_pointer) {
        zwlr_virtual_pointer_v1_frame(virtual_pointer);
        wl_display_flush(display);
    }
} 