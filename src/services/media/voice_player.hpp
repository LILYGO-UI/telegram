#ifndef LILYGO_UI_TELEGRAM_SERVICES_MEDIA_VOICE_PLAYER_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_MEDIA_VOICE_PLAYER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace telegram::media {

using VoiceRequestId = std::uint64_t;

enum class VoicePlaybackState : std::uint8_t {
    idle,
    opening,
    playing,
    stopping,
    stopped,
    finished,
    error,
};

struct VoicePlaybackEvent {
    VoiceRequestId request_id = 0;
    VoicePlaybackState state  = VoicePlaybackState::idle;
    std::string source_path;
    std::string message;
};

class VoicePlayer {
public:
    using Callback = std::function<void(const VoicePlaybackEvent &)>;

    explicit VoicePlayer(std::string alsa_device = "default");
    ~VoicePlayer();

    VoicePlayer(const VoicePlayer &)            = delete;
    VoicePlayer &operator=(const VoicePlayer &) = delete;
    VoicePlayer(VoicePlayer &&)                 = delete;
    VoicePlayer &operator=(VoicePlayer &&)      = delete;

    // Starts playback on the worker thread. Starting another file cancels the
    // current request. Status callbacks run on the worker thread, must not call
    // LVGL directly, and must not destroy this player. Zero means the request
    // was rejected synchronously.
    VoiceRequestId play_file(std::string path, Callback callback, std::string *error = nullptr);

    // Requests prompt asynchronous cancellation; destruction waits until the
    // worker and any callback already in progress have completed.
    void stop() noexcept;

    [[nodiscard]] VoicePlaybackState state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telegram::media

#endif
