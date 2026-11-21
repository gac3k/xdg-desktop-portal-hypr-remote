// WaylandClipboard: see the design comment in clipboard.h.
#include "clipboard.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <wayland-client.h>
#include "ext-data-control-v1-client-protocol.h"

namespace {

bool has_marker(const std::vector<std::string>& mimes) {
    return std::find(mimes.begin(), mimes.end(), kOwnedMarker) != mimes.end();
}

std::string join(const std::vector<std::string>& mimes) {
    std::string out;
    for (const auto& m : mimes) {
        if (!out.empty()) out += ", ";
        out += m;
    }
    return out.empty() ? "(none)" : out;
}

} // namespace

bool valid_mime_type(const std::string& mime) {
    if (mime.empty() || mime.size() > kMaxMimeLen) return false;
    for (unsigned char c : mime)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

// ---- Listeners (all run on the Wayland thread) ------------------------------

struct WaylandClipboard::Listeners {
    static void registry_global(void* data, wl_registry* registry, uint32_t name,
                                const char* interface, uint32_t /*version*/) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self->seat_) {
            self->seat_ = static_cast<wl_seat*>(
                wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (std::strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 &&
                   !self->manager_) {
            self->manager_ = static_cast<ext_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
        }
    }
    static void registry_global_remove(void*, wl_registry*, uint32_t) {}

    static void offer_offer(void* data, ext_data_control_offer_v1* offer, const char* mime) {
        auto* self = static_cast<WaylandClipboard*>(data);
        auto it = self->offers_.find(offer);
        // Some apps (Chromium) offer a type twice; keep the list unique.
        if (!valid_mime_type(mime)) return;
        if (it != self->offers_.end() && it->second.size() >= kMaxMimeTypes) return;
        if (it != self->offers_.end() &&
            std::find(it->second.begin(), it->second.end(), mime) == it->second.end())
            it->second.emplace_back(mime);
    }

    static void device_data_offer(void* data, ext_data_control_device_v1*,
                                  ext_data_control_offer_v1* offer) {
        auto* self = static_cast<WaylandClipboard*>(data);
        self->offers_[offer] = {};
        ext_data_control_offer_v1_add_listener(offer, &offer_listener, self);
    }

    static void device_selection(void* data, ext_data_control_device_v1*,
                                 ext_data_control_offer_v1* offer) {
        static_cast<WaylandClipboard*>(data)->on_selection(offer);
    }

    static void device_finished(void* data, ext_data_control_device_v1*) {
        auto* self = static_cast<WaylandClipboard*>(data);
        std::cerr << "clipboard: data-control device finished; clipboard disabled" << std::endl;
        self->available_ = false;
        if (self->device_) {
            ext_data_control_device_v1_destroy(self->device_);
            self->device_ = nullptr;
        }
        std::lock_guard<std::mutex> lock(self->mtx_);
        self->foreign_mimes_.clear();
    }

    static void device_primary_selection(void* data, ext_data_control_device_v1*,
                                         ext_data_control_offer_v1* offer) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (offer && offer != self->selection_offer_) self->destroy_offer(offer);
    }

    static void source_send(void* data, ext_data_control_source_v1* source,
                            const char* mime, int32_t fd) {
        static_cast<WaylandClipboard*>(data)->on_send(source, mime, fd);
    }

    static void source_cancelled(void* data, ext_data_control_source_v1* source) {
        auto* self = static_cast<WaylandClipboard*>(data);
        if (source == self->source_) {
            self->source_ = nullptr;
            self->claimed_mimes_.clear();
        }
        ext_data_control_source_v1_destroy(source);
    }

    static constexpr wl_registry_listener registry_listener{
        registry_global, registry_global_remove};
    static constexpr ext_data_control_offer_v1_listener offer_listener{offer_offer};
    static constexpr ext_data_control_device_v1_listener device_listener{
        device_data_offer, device_selection, device_finished, device_primary_selection};
    static constexpr ext_data_control_source_v1_listener source_listener{
        source_send, source_cancelled};
};

// ---- Public API ---------------------------------------------------------------

WaylandClipboard::~WaylandClipboard() {
    stop();
}

bool WaylandClipboard::init(ClipboardCallbacks cb) {
    cb_ = std::move(cb);

    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::cerr << "clipboard: data-control unavailable (cannot connect to Wayland display)"
                  << std::endl;
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &Listeners::registry_listener, this);
    if (wl_display_roundtrip(display_) < 0 || !seat_ || !manager_) {
        std::cerr << "clipboard: data-control unavailable ("
                  << (!manager_ ? "no ext_data_control_manager_v1" : "no wl_seat")
                  << "); clipboard sharing disabled" << std::endl;
        teardown_wayland();
        return false;
    }

    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0) {
        std::cerr << "clipboard: data-control unavailable (eventfd: " << std::strerror(errno)
                  << ")" << std::endl;
        teardown_wayland();
        return false;
    }

    device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
    ext_data_control_device_v1_add_listener(device_, &Listeners::device_listener, this);
    wl_display_flush(display_);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_exited_ = false;
    }
    available_ = true;
    thread_ = std::thread([this] { run(); });
    std::cout << "clipboard: ext-data-control ready" << std::endl;
    return true;
}

void WaylandClipboard::stop() {
    if (thread_.joinable()) {
        push_command(Command{Command::Stop, {}, -1});
        thread_.join();
    }
    available_ = false;
    if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
    }
}

void WaylandClipboard::claim(std::vector<std::string> mimes) {
    if (!available_) return;
    size_t dropped = 0;
    std::vector<std::string> ok;
    for (auto& m : mimes) {
        if (!valid_mime_type(m) || ok.size() >= kMaxMimeTypes) { ++dropped; continue; }
        ok.push_back(std::move(m));
    }
    if (dropped > 0)
        std::cerr << "clipboard: dropped " << dropped << " invalid MIME type(s)" << std::endl;
    if (ok.empty()) return;
    push_command(Command{Command::Claim, std::move(ok), -1});
}

int WaylandClipboard::take_write_fd(uint32_t serial) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = pending_.find(serial);
    if (it == pending_.end()) {
        std::cerr << "clipboard: remote->local serial " << serial
                  << " unknown or expired on SelectionWrite" << std::endl;
        return -1;
    }
    int fd = it->second.fd;
    std::cout << "clipboard: remote->local " << it->second.mime << " serial " << serial
              << " handed over" << std::endl;
    pending_.erase(it);
    return fd;
}

void WaylandClipboard::write_done(uint32_t serial, bool ok) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = pending_.find(serial);
    if (it == pending_.end()) {
        if (std::find(failed_serials_.begin(), failed_serials_.end(), serial) !=
            failed_serials_.end()) {
            std::cout << "clipboard: remote->local serial " << serial
                      << " done after it had already failed (paste got no data)" << std::endl;
            return;
        }
        // Normal case: the fd was handed over by SelectionWrite.
        std::cout << "clipboard: remote->local serial " << serial
                  << (ok ? " done ok" : " failed") << std::endl;
        return;
    }
    // SelectionWriteDone without SelectionWrite (Deskflow does this when it has
    // no data for the mime): end the paste now with empty data.
    std::cout << "clipboard: remote->local " << it->second.mime << " serial " << serial
              << (ok ? " done without data" : " failed (no data)") << std::endl;
    close(it->second.fd);
    pending_.erase(it);
}

int WaylandClipboard::open_read(const std::string& mime) {
    if (!available_ || mime == kOwnedMarker) return -1;
    if (!valid_mime_type(mime)) {
        std::cerr << "clipboard: local->remote invalid MIME type requested" << std::endl;
        return -1;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (loop_exited_ ||
        std::find(foreign_mimes_.begin(), foreign_mimes_.end(), mime) == foreign_mimes_.end()) {
        std::cerr << "clipboard: local->remote " << mime << " not offered" << std::endl;
        return -1;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        std::cerr << "clipboard: local->remote pipe2: " << std::strerror(errno) << std::endl;
        return -1;
    }
    commands_.push_back(Command{Command::Receive, {mime}, fds[1]});
    wake();
    std::cout << "clipboard: local->remote " << mime << " read started" << std::endl;
    return fds[0];
}

void WaylandClipboard::announce() {
    if (!available_) return;
    push_command(Command{Command::Announce, {}, -1});
}

void WaylandClipboard::session_ended() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [serial, p] : pending_) {
            std::cout << "clipboard: remote->local " << p.mime << " serial " << serial
                      << " failed (session ended)" << std::endl;
            note_failed(serial);
            close(p.fd);
        }
        pending_.clear();
    }
    if (available_) push_command(Command{Command::SessionEnded, {}, -1});
}

// ---- Internals ------------------------------------------------------------------

void WaylandClipboard::wake() {
    if (event_fd_ >= 0) eventfd_write(event_fd_, 1);
}

void WaylandClipboard::push_command(Command cmd) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (loop_exited_) {
        if (cmd.fd >= 0) close(cmd.fd);
        return;
    }
    commands_.push_back(std::move(cmd));
    wake();
}

void WaylandClipboard::destroy_offer(ext_data_control_offer_v1* offer) {
    offers_.erase(offer);
    ext_data_control_offer_v1_destroy(offer);
}

void WaylandClipboard::on_selection(ext_data_control_offer_v1* offer) {
    if (selection_offer_ && selection_offer_ != offer) destroy_offer(selection_offer_);
    selection_offer_ = offer;

    std::vector<std::string> mimes;
    if (offer) {
        auto it = offers_.find(offer);
        if (it != offers_.end()) mimes = it->second;
        if (has_marker(mimes)) {
            // Our own selection mirrored back: never report it (#10154).
            std::lock_guard<std::mutex> lock(mtx_);
            foreign_mimes_.clear();
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mtx_);
        foreign_mimes_ = mimes;
    }
    std::cout << "clipboard: local selection " << (offer ? "changed: " : "cleared")
              << (offer ? join(mimes) : "") << std::endl;
    if (cb_.owner_changed) cb_.owner_changed(mimes);
}

void WaylandClipboard::on_send(ext_data_control_source_v1* /*source*/, const char* mime, int fd) {
    if (std::strcmp(mime, kOwnedMarker) == 0) {
        close(fd);
        return;
    }
    if (std::find(claimed_mimes_.begin(), claimed_mimes_.end(), mime) == claimed_mimes_.end()) {
        close(fd);
        std::cerr << "clipboard: remote->local paste of a type we did not offer refused" << std::endl;
        return;
    }
    uint32_t serial;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (pending_.size() >= kMaxPending) {
            close(fd);
            std::cerr << "clipboard: remote->local too many pending pastes; refused" << std::endl;
            return;
        }
        do {
            serial = ++serial_counter_;
        } while (serial == 0 || pending_.count(serial));
        pending_[serial] = Pending{fd, mime,
                                   std::chrono::steady_clock::now() + kTransferTimeout};
    }
    std::cout << "clipboard: remote->local " << mime << " serial " << serial << " requested"
              << std::endl;
    bool ok = cb_.transfer_requested && cb_.transfer_requested(mime, serial);
    if (!ok) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = pending_.find(serial);
        if (it != pending_.end()) {
            std::cout << "clipboard: remote->local " << mime << " serial " << serial
                      << " failed (no clipboard session)" << std::endl;
            close(it->second.fd);
            pending_.erase(it);
        }
    }
}

void WaylandClipboard::run_command(Command& cmd, bool& stop_requested) {
    switch (cmd.kind) {
    case Command::Claim: {
        if (!device_ || !manager_) break;
        auto* src = ext_data_control_manager_v1_create_data_source(manager_);
        ext_data_control_source_v1_add_listener(src, &Listeners::source_listener, this);
        for (const auto& m : cmd.mimes) ext_data_control_source_v1_offer(src, m.c_str());
        ext_data_control_source_v1_offer(src, kOwnedMarker); // last, see design 4
        ext_data_control_device_v1_set_selection(device_, src);
        if (source_) ext_data_control_source_v1_destroy(source_);
        source_ = src;
        claimed_mimes_ = cmd.mimes;
        std::cout << "clipboard: remote selection claimed: " << join(cmd.mimes) << std::endl;
        break;
    }
    case Command::Receive: {
        const std::string& mime = cmd.mimes.front();
        bool listed = false;
        if (selection_offer_) {
            auto it = offers_.find(selection_offer_);
            listed = it != offers_.end() && !has_marker(it->second) &&
                     std::find(it->second.begin(), it->second.end(), mime) != it->second.end();
        }
        if (listed) {
            ext_data_control_offer_v1_receive(selection_offer_, mime.c_str(), cmd.fd);
            wl_display_flush(display_);
        } else {
            std::cout << "clipboard: local->remote " << mime
                      << " failed (selection changed)" << std::endl;
        }
        close(cmd.fd); // libwayland has dup'ed it into the request
        cmd.fd = -1;
        break;
    }
    case Command::Announce: {
        std::vector<std::string> mimes;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            mimes = foreign_mimes_;
        }
        if (!mimes.empty() && cb_.owner_changed) {
            std::cout << "clipboard: announcing current local selection: " << join(mimes)
                      << std::endl;
            cb_.owner_changed(mimes);
        }
        break;
    }
    case Command::SessionEnded:
        if (source_) {
            ext_data_control_source_v1_destroy(source_);
            source_ = nullptr;
            claimed_mimes_.clear();
            std::cout << "clipboard: session ended, remote selection dropped" << std::endl;
        }
        break;
    case Command::Stop:
        stop_requested = true;
        break;
    }
}

void WaylandClipboard::note_failed(uint32_t serial) {
    failed_serials_.push_back(serial);
    while (failed_serials_.size() > 32) failed_serials_.pop_front();
}

void WaylandClipboard::expire_pending() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.deadline <= now) {
            std::cout << "clipboard: remote->local " << it->second.mime << " serial "
                      << it->first << " timed out" << std::endl;
            note_failed(it->first);
            close(it->second.fd);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

int WaylandClipboard::poll_timeout_ms() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (pending_.empty()) return -1;
    auto earliest = pending_.begin()->second.deadline;
    for (const auto& [serial, p] : pending_) earliest = std::min(earliest, p.deadline);
    auto ms = std::chrono::ceil<std::chrono::milliseconds>(
        earliest - std::chrono::steady_clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms, 60000));
}

void WaylandClipboard::run() {
    const int wl_fd = wl_display_get_fd(display_);
    bool stop_requested = false;

    while (!stop_requested) {
        while (wl_display_prepare_read(display_) != 0) {
            if (wl_display_dispatch_pending(display_) < 0) goto display_error;
        }
        short wl_events = POLLIN;
        if (wl_display_flush(display_) < 0 && errno == EAGAIN) wl_events |= POLLOUT;

        pollfd fds[2] = {{wl_fd, wl_events, 0}, {event_fd_, POLLIN, 0}};
        int n = poll(fds, 2, poll_timeout_ms());
        if (n < 0 && errno != EINTR) {
            wl_display_cancel_read(display_);
            std::cerr << "clipboard: poll: " << std::strerror(errno) << std::endl;
            break;
        }
        if (n > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(display_) < 0) goto display_error;
        } else {
            wl_display_cancel_read(display_);
        }
        if (n > 0 && (fds[0].revents & (POLLERR | POLLHUP))) goto display_error;
        if (wl_display_dispatch_pending(display_) < 0) goto display_error;

        {
            eventfd_t value;
            eventfd_read(event_fd_, &value); // EAGAIN when not signalled; that's fine
        }
        std::deque<Command> batch;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            batch.swap(commands_);
        }
        for (auto& cmd : batch) run_command(cmd, stop_requested);
        wl_display_flush(display_);
        expire_pending();
    }
    goto done;

display_error:
    std::cerr << "clipboard: Wayland connection error (" << std::strerror(errno)
              << "); clipboard disabled" << std::endl;

done:
    available_ = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_exited_ = true;
        for (auto& [serial, p] : pending_) {
            note_failed(serial);
            close(p.fd);
        }
        pending_.clear();
        for (auto& c : commands_)
            if (c.fd >= 0) close(c.fd);
        commands_.clear();
        foreign_mimes_.clear();
    }
    teardown_wayland();
}

void WaylandClipboard::teardown_wayland() {
    for (auto& [offer, mimes] : offers_) ext_data_control_offer_v1_destroy(offer);
    offers_.clear();
    selection_offer_ = nullptr;
    if (source_) { ext_data_control_source_v1_destroy(source_); source_ = nullptr; }
    claimed_mimes_.clear();
    if (device_) { ext_data_control_device_v1_destroy(device_); device_ = nullptr; }
    if (manager_) { ext_data_control_manager_v1_destroy(manager_); manager_ = nullptr; }
    if (seat_) { wl_seat_destroy(seat_); seat_ = nullptr; }
    if (registry_) { wl_registry_destroy(registry_); registry_ = nullptr; }
    if (display_) {
        wl_display_flush(display_);
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}
