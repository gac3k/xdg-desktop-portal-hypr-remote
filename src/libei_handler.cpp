#include "libei_handler.h"
#include "wayland_virtual_keyboard.h"
#include "wayland_virtual_pointer.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <cerrno>

extern "C" {
#include <wayland-client-protocol.h>
}

LibEIHandler::LibEIHandler()
    : ei_context(nullptr), seat(nullptr), keyboard(nullptr), pointer(nullptr), running(false) {
}

LibEIHandler::~LibEIHandler() {
    cleanup();
}

bool LibEIHandler::init(WaylandVirtualKeyboard* kb, WaylandVirtualPointer* ptr) {
    keyboard = kb;
    pointer = ptr;
    
    std::cout << "Initializing LibEI Handler..." << std::endl;
    
    // Create a new EI receiver context (we receive events from remote clients)
    ei_context = ei_new_receiver(this);
    if (!ei_context) {
        std::cerr << "Failed to create EI receiver context" << std::endl;
        return false;
    }
    
    // Configure the name for this context
    ei_configure_name(ei_context, "Hyprland Remote Desktop Portal");
    
    std::cout << "EI receiver context created for portal file descriptor sharing" << std::endl;
    std::cout << "✓ LibEI Handler initialized successfully" << std::endl;
    running = true;
    return true;
}

void LibEIHandler::cleanup() {
    running = false;
    
    if (seat) {
        ei_seat_unref(seat);
        seat = nullptr;
    }
    
    if (ei_context) {
        ei_unref(ei_context);
        ei_context = nullptr;
    }
}

void LibEIHandler::run() {
    if (!ei_context) {
        std::cout << "LibEI Handler not initialized, cannot run" << std::endl;
        return;
    }
    
    // running is set in init(), so a stop() issued before this thread gets here sticks.
    std::cout << "LibEI Handler running and processing events..." << std::endl;
    
    int ei_fd = ei_get_fd(ei_context);
    if (ei_fd < 0) {
        std::cerr << "Failed to get EI file descriptor" << std::endl;
        return;
    }
    
    while (running) {
        // Use select to wait for events with timeout
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ei_fd, &fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout
        
        int result = select(ei_fd + 1, &fds, nullptr, nullptr, &timeout);
        
        if (result > 0 && FD_ISSET(ei_fd, &fds)) {
            // Dispatch events
            ei_dispatch(ei_context);
            struct ei_event* event;
            while ((event = ei_get_event(ei_context)) != nullptr) {
                handle_event(event);
                ei_event_unref(event);
            }
        } else if (result < 0) {
            std::cerr << "Error in select(): " << strerror(errno) << std::endl;
            break;
        }
        // If result == 0, it's just a timeout, continue the loop
    }
    
    std::cout << "LibEI Handler stopped processing events" << std::endl;
}

void LibEIHandler::stop() {
    running = false;
    std::cout << "LibEI Handler stop requested" << std::endl;
}

void LibEIHandler::handle_event(struct ei_event* event) {
    std::lock_guard<std::mutex> lock(input_mutex);
    enum ei_event_type type = ei_event_get_type(event);
    
    switch (type) {
        case EI_EVENT_CONNECT:
            std::cout << "EI: Client connected" << std::endl;
            break;
            
        case EI_EVENT_DISCONNECT:
            std::cout << "EI: Client disconnected" << std::endl;
            break;
            
        case EI_EVENT_SEAT_ADDED:
            std::cout << "EI: Seat added" << std::endl;
            seat = ei_event_get_seat(event);
            ei_seat_ref(seat);
            break;
            
        case EI_EVENT_SEAT_REMOVED:
            std::cout << "EI: Seat removed" << std::endl;
            if (seat) {
                ei_seat_unref(seat);
                seat = nullptr;
            }
            break;
            
        case EI_EVENT_DEVICE_ADDED:
            std::cout << "EI: Device added" << std::endl;
            break;
            
        case EI_EVENT_DEVICE_REMOVED:
            std::cout << "EI: Device removed" << std::endl;
            break;
            
        case EI_EVENT_POINTER_MOTION:
            handle_pointer_event(event);
            break;
            
        case EI_EVENT_POINTER_MOTION_ABSOLUTE:
            handle_pointer_event(event);
            break;
            
        case EI_EVENT_BUTTON_BUTTON:
            handle_pointer_event(event);
            break;
            
        case EI_EVENT_SCROLL_DELTA:
        case EI_EVENT_SCROLL_DISCRETE:
            handle_pointer_event(event);
            break;
            
        case EI_EVENT_KEYBOARD_KEY:
            handle_keyboard_event(event);
            break;
            
        case EI_EVENT_FRAME:
            // Frame events group related events together
            // We can flush any pending events here
            break;
            
        default:
            std::cout << "EI: Unhandled event type: " << type << std::endl;
            break;
    }
}

void LibEIHandler::handle_keyboard_event(struct ei_event* event) {
    if (!keyboard) {
        return;
    }
    
    enum ei_event_type type = ei_event_get_type(event);
    
    if (type == EI_EVENT_KEYBOARD_KEY) {
        uint32_t keycode = ei_event_keyboard_get_key(event);
        bool is_press = ei_event_keyboard_get_key_is_press(event);
        
        
        // Get current time for wayland events
        uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        
        // Forward to virtual keyboard
        keyboard->send_key(time, keycode, is_press ? 1 : 0);
    }
}

void LibEIHandler::handle_pointer_event(struct ei_event* event) {
    if (!pointer) {
        return;
    }
    
    enum ei_event_type type = ei_event_get_type(event);
    
    // Get current time for wayland events
    uint32_t time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    switch (type) {
        case EI_EVENT_POINTER_MOTION: {
            double dx = ei_event_pointer_get_dx(event);
            double dy = ei_event_pointer_get_dy(event);
            
            
            // Forward relative motion to virtual pointer
            pointer->send_motion(time, dx, dy);
            pointer->send_frame();
            break;
        }
        
        case EI_EVENT_POINTER_MOTION_ABSOLUTE: {
            double x = ei_event_pointer_get_absolute_x(event);
            double y = ei_event_pointer_get_absolute_y(event);
            
            
            // For absolute motion, we need screen dimensions
            // For now, assume 1920x1080 - this should be dynamically determined
            uint32_t screen_width = 1920;
            uint32_t screen_height = 1080;
            
            pointer->send_motion_absolute(time, 
                static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                screen_width, screen_height);
            pointer->send_frame();
            break;
        }
        
        case EI_EVENT_BUTTON_BUTTON: {
            uint32_t button = ei_event_button_get_button(event);
            bool is_press = ei_event_button_get_is_press(event);
            
            
            // Forward button event to virtual pointer
            pointer->send_button(time, button, is_press ? 1 : 0);
            pointer->send_frame();
            break;
        }
        
        case EI_EVENT_SCROLL_DELTA: {
            double dx = ei_event_scroll_get_dx(event);
            double dy = ei_event_scroll_get_dy(event);
            
            
            // Send scroll events for both axes if non-zero
            if (dx != 0.0) {
                pointer->send_axis(time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, dx);
            }
            if (dy != 0.0) {
                pointer->send_axis(time, WL_POINTER_AXIS_VERTICAL_SCROLL, dy);
            }
            pointer->send_frame();
            break;
        }
        
        case EI_EVENT_SCROLL_DISCRETE: {
            int32_t dx = ei_event_scroll_get_discrete_dx(event);
            int32_t dy = ei_event_scroll_get_discrete_dy(event);
            
            
            pointer->send_axis_discrete(time, dx, dy);
            pointer->send_frame();
            break;
        }
        
        default:
            std::cout << "EI: Unhandled pointer event type: " << type << std::endl;
            break;
    }
} 