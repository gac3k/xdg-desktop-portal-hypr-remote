#pragma once

#include <atomic>
#include <mutex>

extern "C" {
#include <libei.h>
}

class WaylandVirtualKeyboard;
class WaylandVirtualPointer;

class LibEIHandler {
public:
    LibEIHandler();
    ~LibEIHandler();
    
    bool init(WaylandVirtualKeyboard* kb, WaylandVirtualPointer* ptr);
    void cleanup();
    void run();
    void stop();
    bool is_running() const { return running.load(); }
    
    // Public access to ei_context for portal integration
    struct ei* ei_context;
    
    // Public access to virtual input devices for portal integration
    WaylandVirtualKeyboard* keyboard;
    WaylandVirtualPointer* pointer;
    
    // Public event handling for portal integration
    // Serialises all input injection (modifier state, g_abs, virtual devices).
    // Lock order: input_mutex first, then g_layout_mutex; never take it while
    // holding g_layout_mutex or sessions_mtx; clipboard code never takes it.
    std::mutex input_mutex;

    void handle_event(struct ei_event* event);
    void handle_keyboard_event(struct ei_event* event);
    void handle_pointer_event(struct ei_event* event);
    
private:
    struct ei_seat* seat;
    
    std::atomic<bool> running;
}; 