#pragma once

/*
 * WaylandClipboard: clipboard sharing for org.freedesktop.impl.portal.Clipboard.
 * DESIGN (Phase 1 of .claude/plans/CLIPBOARD-PLAN.md). Phase 2 implements this;
 * if the code has to differ, update this comment in the same commit.
 *
 * 1. COMPONENTS
 *  - WaylandClipboard (clipboard.{h,cpp}) owns its own wl_display, registry, the
 *    first wl_seat (v1), ext_data_control_manager_v1 (v1) and the device. init()
 *    connects, does one roundtrip to bind, calls get_data_device, then starts one
 *    thread; from then on every Wayland call is made on that thread. Loop:
 *    prepare_read (dispatch_pending until it succeeds), flush, poll {display fd,
 *    eventfd(EFD_CLOEXEC|EFD_NONBLOCK)} with timeout = earliest pending deadline
 *    (else -1), read_events or cancel_read, dispatch_pending, drain the eventfd and
 *    run queued commands, flush, expire deadlines. Other threads only lock `mtx`,
 *    push a Command (Claim, Receive, Announce, SessionEnded, Stop) and write the
 *    eventfd. On exit (Stop, or a fatal display error, which also clears
 *    available) the thread sets loop_exited_, closes every pending and queued fd,
 *    destroys all Wayland objects and disconnects; a command pushed after that has
 *    its fd closed at once.
 *  - Portal: the D-Bus side. Its handlers run on the sdbus event-loop thread. It
 *    owns the session table and turns the callbacks below into signals.
 *
 * 2. SIGNAL EMISSION (checked against installed sdbus-cpp 2.3.1-1)
 *  - /usr/include/sdbus-c++/IObject.h:57-59: "creating and sending asynchronous
 *    method replies, as well as creating and emitting signals, is thread-safe by
 *    design." So the callbacks emit straight from the Wayland thread:
 *    object->createSignal(InterfaceName, SignalName), `sig << ...`,
 *    object->emitSignal(sig), wrapped in try/catch(const sdbus::Error&).
 *  - IConnection.h:56-57: the connection is "thread-aware, but not thread-safe",
 *    so the Wayland thread touches only `object`, never `connection`.
 *    IObject.h:469-470: createObject is safe on a connection that is already
 *    running its event loop (used for Session objects in 3).
 *  - libsdbus-c++.so.2.3.1 imports pthread_mutex_lock and eventfd_write (a bus lock
 *    plus loop wake-up). Handlers run under that lock (ThreadSanitizer confirms
 *    it), so: never call a callback or emitSignal, and never create or destroy an
 *    sdbus object, while holding WaylandClipboard::mtx or Portal's sessions_mtx.
 *    Copy what is needed, unlock, then emit.
 *  - `object` must outlive the thread: clipboard.stop() runs before portal.cleanup().
 *
 * 3. SESSIONS (Portal): std::map<std::string, SessionState> under sessions_mtx,
 *    SessionState{bool clipboard_requested, started; unique_ptr<IObject> obj}.
 *  - xdg-desktop-portal 1.22.1 (src/xdp-session.c, xdp_session_close) calls
 *    impl.portal.Session.Close synchronously at session_handle, both on Close and
 *    when the client disconnects. Today that gets UnknownObject. Decision:
 *    CreateSession exports org.freedesktop.impl.portal.Session at session_handle
 *    (method Close, property version=1). Close replies, erases the entry and moves
 *    obj into `retired`, because an object must not be destroyed inside its own
 *    callback. `retired` is cleared at the next CreateSession.
 *  - RequestClipboard and Start on an unknown handle create the entry.
 *  - The clipboard session is the most recently Started entry with both flags set,
 *    kept as `clip_session` ("" = none). Only it receives signals and may use the
 *    clipboard methods. Any other handle gets org.freedesktop.portal.Error.Failed.
 *    When it closes: clip_session = "" and clipboard->session_ended(). Entries left
 *    behind by a frontend crash (no Close) are harmless; the next Start takes over.
 *
 * 4. MAC -> LAPTOP (Synergy owns the selection)
 *  - SetSelection(s, {mime_types}): strip kOwnedMarker, ignore an empty list,
 *    claim(mimes), reply at once. Wayland thread: create_data_source, offer each
 *    mime and then kOwnedMarker *last* (so "first type" pickers never choose it),
 *    set_selection, destroy our previous source.
 *  - source.send(mime, fd): mime == kOwnedMarker -> close(fd), no signal. Otherwise
 *    serial = ++counter (uint32, skip 0), pending[serial] = {fd, mime, now + 5 s},
 *    unlock, transfer_requested(mime, serial): Portal emits SelectionTransfer to
 *    clip_session. If there is none it returns false and the fd is closed at once.
 *  - SelectionWrite(s, serial): OPTION (b), direct hand-off. take_write_fd()
 *    removes the entry under mtx and returns its fd; reply with
 *    sdbus::UnixFd{fd, sdbus::adopt_fd} (Types.h:329), whose destructor closes our
 *    copy after the reply is sent. (The plain UnixFd{fd} ctor dups, Types.h:324.)
 *    Unknown or expired serial -> D-Bus error.
 *    Why (b): Synergy bounds its own writes (poll timeout; synergy-core logs "timed
 *    out writing clipboard selection"), always closes the fd, and limits size with
 *    its clipboardSharingSize setting. The portal then never touches clipboard
 *    bytes and has no pump threads, so nothing on our side can block (#10292) or
 *    outlive its objects. Cost: no portal-side size cap or write timeout once the
 *    fd has been handed over.
 *  - SelectionWriteDone(s, serial, ok): if the entry is still pending, close its fd
 *    and erase it. (Deskflow sends ok=false *without* calling SelectionWrite when it
 *    has no data for the mime.) Otherwise it is a no-op. Always reply empty.
 *  - Timeout: the loop closes and erases entries past their deadline, so the
 *    pasting app gets empty data instead of hanging (#10401).
 *  - source.cancelled: destroy the source. Fds already handed over are unaffected.
 *    Pending entries keep their deadline.
 *  - session_ended(): close every pending fd now and destroy our source (the
 *    compositor clears the selection, so later pastes don't wait on a dead session).
 *
 * 5. LAPTOP -> MAC (a local app owns the selection)
 *  - data_offer: track the offer and collect its mimes. selection(offer): destroy
 *    the previous selection offer. If the offer lists kOwnedMarker it is ours:
 *    keep it, set foreign = {} and emit nothing (decided; Deskflow ignores
 *    session_is_owner=true anyway). Otherwise foreign = its mimes (under mtx), then
 *    owner_changed(mimes) -> SelectionOwnerChanged(s, {mime_types,
 *    session_is_owner=false}). Emit for every foreign offer, even one with the same
 *    mime list, because it is new content. selection(null): foreign = {},
 *    owner_changed({}). primary_selection: destroy that offer. finished: destroy the
 *    device, available = false.
 *  - Deskflow reacts by reading every type it supports (image/png,
 *    text/plain;charset=utf-8, text/plain) at once, polling 200 ms per chunk.
 *  - SelectionRead(s, mime): mime not in `foreign` -> D-Bus error. Otherwise
 *    pipe2(O_CLOEXEC), queue Receive{mime, write end}, and reply with the read end
 *    adopted. Wayland thread: if the current foreign offer still lists the mime,
 *    receive and flush. Close the write end either way (a race just yields EOF).
 *  - announce(), queued after Start's reply: the loop re-emits owner_changed(foreign)
 *    if it is non-empty, so a copy made before Synergy connected is offered.
 *
 * 6. LIMITS: with (b) there is no portal-side size cap (we never see the bytes);
 *    Synergy's clipboardSharingSize applies. 5 s deadline for SelectionWrite. main
 *    still sets signal(SIGPIPE, SIG_IGN): the EIS bridge write()s to sockets.
 * 7. LOGGING: lifecycle events, plus one line per transfer with direction, mime,
 *    serial and outcome (handed over / done ok / failed / timed out). Sizes aren't
 *    known under (b). Never log contents. Required lines: "clipboard requested",
 *    "Start: clipboard_enabled=true|false", "data-control unavailable" (once).
 * 8. FALLBACK: ext-data-control only. If there is no display, seat or manager,
 *    init() logs "data-control unavailable" and returns false. The Clipboard
 *    interface is still exported, but its methods return errors and Start omits
 *    clipboard_enabled. Mouse and keyboard are unaffected.
 * 9. INIT: main creates WaylandClipboard after the keyboard and pointer, and calls
 *    portal.init(&libei, &clipboard). That registers both vtables and then calls
 *    clipboard->init(callbacks); a false return is logged and ignored. Shutdown:
 *    portal.stop() + join, clipboard.stop(), libei stop + join, then cleanups.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_data_control_manager_v1;
struct ext_data_control_device_v1;
struct ext_data_control_source_v1;
struct ext_data_control_offer_v1;

inline constexpr const char* kOwnedMarker = "application/x-hypr-remote-owned";

// Called on the Wayland thread, never with a WaylandClipboard lock held.
struct ClipboardCallbacks {
    // A foreign selection appeared (empty = cleared). Never called for our own.
    std::function<void(const std::vector<std::string>& mimes)> owner_changed;
    // A local app pastes from our source. Return false if no session can answer.
    std::function<bool(const std::string& mime, uint32_t serial)> transfer_requested;
};

class WaylandClipboard {
public:
    WaylandClipboard() = default;
    ~WaylandClipboard();                              // calls stop()
    WaylandClipboard(const WaylandClipboard&) = delete;
    WaylandClipboard& operator=(const WaylandClipboard&) = delete;

    bool init(ClipboardCallbacks cb);                 // false if data-control unsupported
    void stop();                                      // idempotent; joins, closes all fds
    bool available() const { return available_.load(); }

    // Any thread (the D-Bus thread in practice).
    void claim(std::vector<std::string> mimes);       // SetSelection
    int  take_write_fd(uint32_t serial);              // SelectionWrite: fd to adopt, or -1
    void write_done(uint32_t serial, bool ok);        // SelectionWriteDone
    int  open_read(const std::string& mime);          // SelectionRead: read end to adopt, or -1
    void announce();                                  // re-emit current foreign selection
    void session_ended();                             // fail pending transfers, drop our source

private:
    struct Listeners;                                 // static Wayland listener callbacks
    struct Pending { int fd; std::string mime; std::chrono::steady_clock::time_point deadline; };
    struct Command { enum Kind { Claim, Receive, Announce, SessionEnded, Stop } kind;
                     std::vector<std::string> mimes; int fd = -1; };   // Receive: mimes = {mime}
    static constexpr std::chrono::seconds kTransferTimeout{5};

    void run();                                       // the Wayland thread
    void push_command(Command cmd);                   // closes cmd.fd if the loop has exited
    void wake();                                      // write the eventfd
    void run_command(Command& cmd, bool& stop_requested);
    void on_selection(ext_data_control_offer_v1* offer);
    void on_send(ext_data_control_source_v1* source, const char* mime, int fd);
    void destroy_offer(ext_data_control_offer_v1* offer);
    void expire_pending();
    int  poll_timeout_ms();
    void teardown_wayland();                          // Wayland thread, or init() before it starts

    ClipboardCallbacks cb_;
    std::atomic<bool> available_{false};
    std::thread thread_;
    int event_fd_ = -1;

    std::mutex mtx_;                                  // guards everything below
    bool loop_exited_ = true;                         // no thread to run commands: close fds at once
    std::deque<Command> commands_;
    std::map<uint32_t, Pending> pending_;
    uint32_t serial_counter_ = 0;
    std::vector<std::string> foreign_mimes_;          // copy of the current foreign offer's types

    // Wayland thread only.
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    ext_data_control_manager_v1* manager_ = nullptr;
    ext_data_control_device_v1* device_ = nullptr;
    ext_data_control_source_v1* source_ = nullptr;    // our current source, if any
    ext_data_control_offer_v1* selection_offer_ = nullptr;
    std::map<ext_data_control_offer_v1*, std::vector<std::string>> offers_;
};
