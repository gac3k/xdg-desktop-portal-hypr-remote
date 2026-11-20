#include "portal.h"
#include "wayland_virtual_keyboard.h"
#include "wayland_virtual_pointer.h"
#include "libei_handler.h"
#include "clipboard.h"
#include <iostream>
#include <thread>
#include <signal.h>
#include <csignal>
#include <chrono>

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t received_signal = 0;

// Async-signal-safe: only store the flags, no I/O.
void signal_handler(int signal) {
    received_signal = signal;
    running = 0;
}

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // Writes to a socket or pipe whose reader has gone must fail with EPIPE,
    // not kill the portal (EIS bridge, clipboard fds).
    signal(SIGPIPE, SIG_IGN);
    
    std::cout << "Hyprland Remote Desktop Portal starting..." << std::endl;
    
    // Initialize components
    WaylandVirtualKeyboard waylandVK;
    WaylandVirtualPointer waylandVP;
    LibEIHandler libeiHandler;
    Portal portal;
    // Declared after portal so that it is destroyed (and its thread stopped) first.
    WaylandClipboard clipboard;
    
    // Initialize Wayland virtual keyboard
    if (!waylandVK.init()) {
        std::cerr << "Failed to initialize Wayland virtual keyboard" << std::endl;
        return 1;
    }
    std::cout << "✓ Virtual keyboard initialized" << std::endl;
    
    // Initialize Wayland virtual pointer
    if (!waylandVP.init()) {
        std::cerr << "Failed to initialize Wayland virtual pointer" << std::endl;
        waylandVK.cleanup();
        return 1;
    }
    std::cout << "✓ Virtual pointer initialized" << std::endl;
    
    // Initialize libei handler
    if (!libeiHandler.init(&waylandVK, &waylandVP)) {
        std::cerr << "Failed to initialize LibEI handler" << std::endl;
        waylandVP.cleanup();
        waylandVK.cleanup();
        return 1;
    }
    std::cout << "✓ LibEI handler initialized" << std::endl;
    
    // Start LibEI handler in background thread
    std::thread libei_thread([&libeiHandler]() {
        libeiHandler.run();
    });
    
    std::cout << "✓ LibEI handler started and ready for connections" << std::endl;
    
    // Initialize portal; it also initializes the clipboard (clipboard.h, item 9)
    if (!portal.init(&libeiHandler, &clipboard)) {
        std::cerr << "Failed to initialize D-Bus portal" << std::endl;
        // libei_thread is joinable here; destroying it would call std::terminate.
        libeiHandler.stop();
        libei_thread.join();
        libeiHandler.cleanup();
        waylandVP.cleanup();
        waylandVK.cleanup();
        return 1;
    }
    std::cout << "✓ D-Bus portal initialized" << std::endl;
    
    std::cout << "\n🚀 Hyprland Remote Desktop Portal is ready!" << std::endl;
    std::cout << "Portal available at: org.freedesktop.impl.portal.desktop.hypr-remote" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    
    // Start portal in separate thread
    std::thread portal_thread([&portal]() {
        portal.run();
    });
    
    // Main loop - just wait for shutdown signal
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "\nReceived signal " << static_cast<int>(received_signal) << ", shutting down..." << std::endl;
    std::cout << "\nShutting down components..." << std::endl;
    
    // Stop and cleanup
    portal.stop();
    if (portal_thread.joinable()) {
        portal_thread.join();
    }
    
    // The clipboard thread emits signals on portal's object, so stop it before
    // portal.cleanup() destroys that object.
    clipboard.stop();
    
    libeiHandler.stop();
    if (libei_thread.joinable()) {
        libei_thread.join();
    }
    
    // Cleanup in reverse order
    portal.cleanup();
    libeiHandler.cleanup();
    waylandVP.cleanup();
    waylandVK.cleanup();
    
    std::cout << "✓ Shutdown complete" << std::endl;
    return 0;
} 