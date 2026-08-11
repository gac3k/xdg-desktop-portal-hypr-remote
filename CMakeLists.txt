cmake_minimum_required(VERSION 3.16)
project(xdg-desktop-portal-hypr-remote LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

# Development mode option
option(DEVELOPMENT_MODE "Build in development mode with different service name" ON)

if(DEVELOPMENT_MODE)
    add_definitions(-DDEVELOPMENT_MODE)
    message(STATUS "Building in DEVELOPMENT mode - will use .dev service name")
endif()



find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(LIBEI REQUIRED libei-1.0)
pkg_check_modules(LIBEIS REQUIRED libeis-1.0)
pkg_check_modules(SDBUSCPP REQUIRED sdbus-c++)

# Find wayland-scanner
find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found")
endif()

# Protocol generation
set(PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protocols")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

# Virtual keyboard protocol
set(VIRTUAL_KEYBOARD_XML "${PROTOCOL_DIR}/virtual-keyboard-unstable-v1.xml")
set(VIRTUAL_KEYBOARD_HEADER "${GENERATED_DIR}/virtual-keyboard-unstable-v1-client-protocol.h")
set(VIRTUAL_KEYBOARD_SOURCE "${GENERATED_DIR}/virtual-keyboard-unstable-v1-protocol.c")

add_custom_command(
    OUTPUT ${VIRTUAL_KEYBOARD_HEADER}
    COMMAND ${WAYLAND_SCANNER} client-header ${VIRTUAL_KEYBOARD_XML} ${VIRTUAL_KEYBOARD_HEADER}
    DEPENDS ${VIRTUAL_KEYBOARD_XML}
    COMMENT "Generating virtual keyboard client header"
)

add_custom_command(
    OUTPUT ${VIRTUAL_KEYBOARD_SOURCE}
    COMMAND ${WAYLAND_SCANNER} private-code ${VIRTUAL_KEYBOARD_XML} ${VIRTUAL_KEYBOARD_SOURCE}
    DEPENDS ${VIRTUAL_KEYBOARD_XML}
    COMMENT "Generating virtual keyboard source"
)

# Virtual pointer protocol
set(VIRTUAL_POINTER_XML "${PROTOCOL_DIR}/wlr-virtual-pointer-unstable-v1.xml")
set(VIRTUAL_POINTER_HEADER "${GENERATED_DIR}/wlr-virtual-pointer-unstable-v1-client-protocol.h")
set(VIRTUAL_POINTER_SOURCE "${GENERATED_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c")


add_custom_command(
    OUTPUT ${VIRTUAL_POINTER_HEADER}
    COMMAND ${WAYLAND_SCANNER} client-header ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_HEADER}
    DEPENDS ${VIRTUAL_POINTER_XML}
    COMMENT "Generating virtual pointer client header"
)

add_custom_command(
    OUTPUT ${VIRTUAL_POINTER_SOURCE}
    COMMAND ${WAYLAND_SCANNER} private-code ${VIRTUAL_POINTER_XML} ${VIRTUAL_POINTER_SOURCE}
    DEPENDS ${VIRTUAL_POINTER_XML}
    COMMENT "Generating virtual pointer source"
)

# Create a library for protocol sources with C linkage
add_library(wayland_protocols STATIC
    ${VIRTUAL_KEYBOARD_SOURCE}
    ${VIRTUAL_POINTER_SOURCE}
)

# Ensure protocol headers are generated before compilation
add_custom_target(generate_protocols DEPENDS 
    ${VIRTUAL_KEYBOARD_HEADER}
    ${VIRTUAL_KEYBOARD_SOURCE}
    ${VIRTUAL_POINTER_HEADER}
    ${VIRTUAL_POINTER_SOURCE}
)
add_dependencies(wayland_protocols generate_protocols)

# Set language to C for this library
set_target_properties(wayland_protocols PROPERTIES
    LINKER_LANGUAGE C
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(wayland_protocols PUBLIC
    ${GENERATED_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)

target_link_libraries(wayland_protocols
    ${WAYLAND_CLIENT_LIBRARIES}
)

include_directories(
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_PROTOCOLS_INCLUDE_DIRS}
    ${LIBEI_INCLUDE_DIRS}
    ${LIBEIS_INCLUDE_DIRS}
    ${SDBUSCPP_INCLUDE_DIRS}
    ${GENERATED_DIR}
)

link_directories(
    ${WAYLAND_CLIENT_LIBRARY_DIRS}
    ${WAYLAND_PROTOCOLS_LIBRARY_DIRS}
    ${LIBEI_LIBRARY_DIRS}
    ${LIBEIS_LIBRARY_DIRS}
    ${SDBUSCPP_LIBRARY_DIRS}
)

# Main executable
add_executable(xdg-desktop-portal-hypr-remote
    src/main.cpp
    src/portal.cpp
    src/libei_handler.cpp
    src/wayland_virtual_keyboard.cpp
    src/wayland_virtual_pointer.cpp
)

# Ensure protocol headers are generated before compilation
add_dependencies(xdg-desktop-portal-hypr-remote generate_protocols)

target_link_libraries(xdg-desktop-portal-hypr-remote
    wayland_protocols
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_PROTOCOLS_LIBRARIES}
    ${LIBEI_LIBRARIES}
    ${LIBEIS_LIBRARIES}
    ${SDBUSCPP_LIBRARIES}
)

# Installation
include(GNUInstallDirs)

# Install the executable
install(TARGETS xdg-desktop-portal-hypr-remote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install portal configuration
install(FILES data/hypr-remote.portal
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/xdg-desktop-portal/portals"
)
set(SYSTEMD_SERVICES
    ON
    CACHE BOOL "Install systemd service file")

# The executable is installed into BINDIR above, so the service files have to
# point there. They used to be substituted with LIBEXECDIR, which pointed at a
# path where nothing was ever installed.
set(BINDIR ${CMAKE_INSTALL_FULL_BINDIR})

# The D-Bus service file is what makes the portal activatable on demand. Without
# it installed, xdg-desktop-portal fails any RemoteDesktop request with
# "The name is not activatable" and the backend is never started.
configure_file(org.freedesktop.impl.portal.desktop.hypr-remote.service.in
               org.freedesktop.impl.portal.desktop.hypr-remote.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.hypr-remote.service
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/services"
)

if(SYSTEMD_SERVICES)
  configure_file(contrib/systemd/xdg-desktop-portal-hypr-remote.service.in
                 contrib/systemd/xdg-desktop-portal-hypr-remote.service @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/contrib/systemd/xdg-desktop-portal-hypr-remote.service
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user"
  )
endif()

# Test executable for virtual input
add_executable(test-virtual-input
    test_virtual_input.cpp
    src/wayland_virtual_keyboard.cpp
    src/wayland_virtual_pointer.cpp
)

# Ensure protocol headers are generated before compilation
add_dependencies(test-virtual-input generate_protocols)

target_link_libraries(test-virtual-input
    wayland_protocols
    ${WAYLAND_CLIENT_LIBRARIES}
)

target_include_directories(test-virtual-input PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${GENERATED_DIR}
) 