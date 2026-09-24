#include "services/media/voice_player.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace telegram::media {
namespace {

struct OggOpusFile;
struct AlsaPcm;

class DynamicLibrary {
public:
    DynamicLibrary() = default;

    ~DynamicLibrary()
    {
        if (handle_) dlclose(handle_);
    }

    DynamicLibrary(const DynamicLibrary &)            = delete;
    DynamicLibrary &operator=(const DynamicLibrary &) = delete;

    bool open(std::initializer_list<const char *> candidates, const char *description, std::string *error)
    {
        for (const char *candidate : candidates) {
            handle_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
            if (handle_) return true;
        }
        const char *detail = dlerror();
        *error             = std::string(description) + " is unavailable";
        if (detail && detail[0]) *error += std::string(": ") + detail;
        return false;
    }

    template <typename Function>
    bool load(Function *function, const char *name, std::string *error)
    {
        static_assert(sizeof(Function) == sizeof(void *), "POSIX function pointers must fit in dlsym pointers");
        dlerror();
        void *address     = dlsym(handle_, name);
        const char *issue = dlerror();
        if (issue || !address) {
            *error = std::string("missing runtime symbol ") + name;
            if (issue && issue[0]) *error += std::string(": ") + issue;
            return false;
        }
        std::memcpy(function, &address, sizeof(address));
        return true;
    }

private:
    void *handle_ = nullptr;
};

struct OpusFileApi {
    using OpenFile   = OggOpusFile *(*)(const char *, int *);
    using ReadStereo = int (*)(OggOpusFile *, std::int16_t *, int);
    using Free       = void (*)(OggOpusFile *);

    bool load(std::string *error)
    {
        return library.open({"libopusfile.so.0", "libopusfile.so"}, "libopusfile", error) &&
               library.load(&open_file, "op_open_file", error) && library.load(&read_stereo, "op_read_stereo", error) &&
               library.load(&free_file, "op_free", error);
    }

    DynamicLibrary library;
    OpenFile open_file     = nullptr;
    ReadStereo read_stereo = nullptr;
    Free free_file         = nullptr;
};

struct AlsaApi {
    using Open             = int (*)(AlsaPcm **, const char *, int, int);
    using SetParams        = int (*)(AlsaPcm *, int, int, unsigned int, unsigned int, int, unsigned int);
    using WriteInterleaved = long (*)(AlsaPcm *, const void *, unsigned long);
    using Recover          = int (*)(AlsaPcm *, int, int);
    using Drain            = int (*)(AlsaPcm *);
    using Drop             = int (*)(AlsaPcm *);
    using Close            = int (*)(AlsaPcm *);
    using ErrorString      = const char *(*)(int);
    using FormatValue      = int (*)(const char *);

    bool load(std::string *error)
    {
        return library.open({"libasound.so.2", "libasound.so"}, "ALSA", error) &&
               library.load(&open, "snd_pcm_open", error) && library.load(&set_params, "snd_pcm_set_params", error) &&
               library.load(&write_interleaved, "snd_pcm_writei", error) &&
               library.load(&recover, "snd_pcm_recover", error) && library.load(&drain, "snd_pcm_drain", error) &&
               library.load(&drop, "snd_pcm_drop", error) && library.load(&close, "snd_pcm_close", error) &&
               library.load(&error_string, "snd_strerror", error) &&
               library.load(&format_value, "snd_pcm_format_value", error);
    }

    DynamicLibrary library;
    Open open                          = nullptr;
    SetParams set_params               = nullptr;
    WriteInterleaved write_interleaved = nullptr;
    Recover recover                    = nullptr;
    Drain drain                        = nullptr;
    Drop drop                          = nullptr;
    Close close                        = nullptr;
    ErrorString error_string           = nullptr;
    FormatValue format_value           = nullptr;
};

class OpusFileHandle {
public:
    OpusFileHandle(OpusFileApi &api, OggOpusFile *file) : api_(api), file_(file)
    {
    }
    ~OpusFileHandle()
    {
        if (file_) api_.free_file(file_);
    }

    [[nodiscard]] OggOpusFile *get() const noexcept
    {
        return file_;
    }

private:
    OpusFileApi &api_;
    OggOpusFile *file_;
};

class AlsaPcmHandle {
public:
    AlsaPcmHandle(AlsaApi &api, AlsaPcm *pcm) : api_(api), pcm_(pcm)
    {
    }
    ~AlsaPcmHandle()
    {
        if (pcm_) api_.close(pcm_);
    }

    [[nodiscard]] AlsaPcm *get() const noexcept
    {
        return pcm_;
    }

private:
    AlsaApi &api_;
    AlsaPcm *pcm_;
};

std::string opus_error(int code)
{
    switch (code) {
        case -128:
            return "read error";
        case -129:
            return "internal fault";
        case -130:
            return "unsupported feature";
        case -131:
            return "invalid argument";
        case -132:
            return "not an Ogg Opus stream";
        case -133:
            return "invalid Ogg Opus header";
        case -134:
            return "unsupported Opus version";
        case -135:
            return "stream contains no audio";
        case -136:
            return "invalid Opus packet";
        case -137:
            return "invalid stream link";
        case -138:
            return "stream is not seekable";
        case -139:
            return "invalid stream timestamp";
        default:
            return "error " + std::to_string(code);
    }
}

std::string alsa_error(const AlsaApi &api, const char *operation, int code)
{
    const char *detail = api.error_string ? api.error_string(code) : nullptr;
    std::string result = operation;
    result += " failed";
    if (detail && detail[0]) result += std::string(": ") + detail;
    return result;
}

}  // namespace

struct VoicePlayer::Impl {
    struct Request {
        VoiceRequestId id = 0;
        std::string path;
        Callback callback;
    };

    struct Outcome {
        VoicePlaybackState state = VoicePlaybackState::error;
        std::string message;
    };

    explicit Impl(std::string device) : alsa_device(std::move(device)), worker([this] { run(); })
    {
    }

    ~Impl()
    {
        shutdown();
    }

    VoiceRequestId play(std::string path, Callback callback, std::string *error)
    {
        if (path.empty()) {
            if (error) *error = "voice path is empty";
            return 0;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (shutting_down.load(std::memory_order_acquire)) {
            if (error) *error = "voice player is shutting down";
            return 0;
        }
        if (pending) {
            if (error) *error = "voice player already has a pending request";
            return 0;
        }

        const auto id = next_id++;
        pending       = Request{id, std::move(path), std::move(callback)};
        active_command.store(id, std::memory_order_release);
        playback_state.store(VoicePlaybackState::opening, std::memory_order_release);
        if (error) error->clear();
        condition.notify_one();
        return id;
    }

    void stop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (shutting_down.load(std::memory_order_acquire)) return;
            active_command.store(next_id++, std::memory_order_release);
            playback_state.store(request_in_progress ? VoicePlaybackState::stopping : VoicePlaybackState::stopped,
                                 std::memory_order_release);
        }
        condition.notify_one();
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!shutting_down.exchange(true, std::memory_order_acq_rel)) {
                pending.reset();
                active_command.store(next_id++, std::memory_order_release);
            }
        }
        condition.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    }

    [[nodiscard]] bool cancelled(VoiceRequestId id) const noexcept
    {
        return shutting_down.load(std::memory_order_acquire) || active_command.load(std::memory_order_acquire) != id;
    }

    void emit(const Request &request, VoicePlaybackState state, std::string message = {}) noexcept
    {
        if (shutting_down.load(std::memory_order_acquire)) return;
        const bool current = active_command.load(std::memory_order_acquire) == request.id;
        if (!current && (state == VoicePlaybackState::opening || state == VoicePlaybackState::playing)) return;
        if (current) {
            playback_state.store(state, std::memory_order_release);
        }
        if (!request.callback) return;
        try {
            request.callback(VoicePlaybackEvent{request.id, state, request.path, std::move(message)});
        } catch (...) {
            // A UI callback cannot be allowed to terminate the audio worker.
        }
    }

    Outcome play_one(const Request &request) noexcept
    {
        if (cancelled(request.id)) return {VoicePlaybackState::stopped, {}};

        std::string error;
        OpusFileApi opus;
        if (!opus.load(&error)) {
            if (cancelled(request.id)) return {VoicePlaybackState::stopped, {}};
            return {VoicePlaybackState::error, std::move(error)};
        }

        AlsaApi alsa;
        if (!alsa.load(&error)) {
            if (cancelled(request.id)) return {VoicePlaybackState::stopped, {}};
            return {VoicePlaybackState::error, std::move(error)};
        }

        int opus_open_error   = 0;
        OggOpusFile *raw_file = opus.open_file(request.path.c_str(), &opus_open_error);
        if (!raw_file) {
            if (cancelled(request.id)) return {VoicePlaybackState::stopped, {}};
            return {VoicePlaybackState::error, "cannot open Ogg Opus voice file: " + opus_error(opus_open_error)};
        }
        OpusFileHandle file(opus, raw_file);

        if (cancelled(request.id)) return {VoicePlaybackState::stopped, {}};

        AlsaPcm *raw_pcm              = nullptr;
        constexpr int kPlaybackStream = 0;
        int result                    = alsa.open(&raw_pcm, alsa_device.c_str(), kPlaybackStream, 0);
        if (result < 0 || !raw_pcm) {
            if (raw_pcm) alsa.close(raw_pcm);
            return {VoicePlaybackState::error, alsa_error(alsa, "opening ALSA playback device", result)};
        }
        AlsaPcmHandle pcm(alsa, raw_pcm);

        const int sample_format = alsa.format_value("S16_LE");
        if (sample_format < 0) {
            return {VoicePlaybackState::error, "ALSA does not support the S16_LE sample format"};
        }

        constexpr int kInterleavedAccess            = 3;
        constexpr unsigned int kChannels            = 2;
        constexpr unsigned int kSampleRate          = 48000;
        constexpr unsigned int kLatencyMicroseconds = 100000;
        result = alsa.set_params(raw_pcm, sample_format, kInterleavedAccess, kChannels, kSampleRate, 1,
                                 kLatencyMicroseconds);
        if (result < 0) {
            return {VoicePlaybackState::error, alsa_error(alsa, "configuring ALSA playback", result)};
        }

        if (cancelled(request.id)) {
            alsa.drop(raw_pcm);
            return {VoicePlaybackState::stopped, {}};
        }
        emit(request, VoicePlaybackState::playing);

        constexpr std::size_t kFramesPerBuffer = 5760;
        std::array<std::int16_t, kFramesPerBuffer * kChannels> samples{};
        unsigned int consecutive_holes = 0;
        for (;;) {
            if (cancelled(request.id)) {
                alsa.drop(raw_pcm);
                return {VoicePlaybackState::stopped, {}};
            }

            const int frames = opus.read_stereo(file.get(), samples.data(), static_cast<int>(samples.size()));
            if (frames == 0) break;
            if (frames == -3) {
                if (++consecutive_holes <= 8) continue;
                return {VoicePlaybackState::error, "too many holes in Ogg Opus stream"};
            }
            if (frames < 0) {
                return {VoicePlaybackState::error, "Ogg Opus decode failed: " + opus_error(frames)};
            }
            consecutive_holes = 0;

            unsigned long written_frames = 0;
            while (written_frames < static_cast<unsigned long>(frames)) {
                if (cancelled(request.id)) {
                    alsa.drop(raw_pcm);
                    return {VoicePlaybackState::stopped, {}};
                }
                const auto *cursor   = samples.data() + written_frames * kChannels;
                const auto remaining = static_cast<unsigned long>(frames) - written_frames;
                const long written   = alsa.write_interleaved(raw_pcm, cursor, remaining);
                if (written < 0) {
                    const int recovered = alsa.recover(raw_pcm, static_cast<int>(written), 1);
                    if (recovered >= 0) continue;
                    return {VoicePlaybackState::error, alsa_error(alsa, "recovering ALSA playback", recovered)};
                }
                if (written == 0) {
                    return {VoicePlaybackState::error, "ALSA playback made no progress"};
                }
                written_frames += static_cast<unsigned long>(written);
            }
        }

        if (cancelled(request.id)) {
            alsa.drop(raw_pcm);
            return {VoicePlaybackState::stopped, {}};
        }
        result = alsa.drain(raw_pcm);
        if (result < 0) {
            return {VoicePlaybackState::error, alsa_error(alsa, "draining ALSA playback", result)};
        }
        return {VoicePlaybackState::finished, {}};
    }

    void run() noexcept
    {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock,
                               [this] { return shutting_down.load(std::memory_order_acquire) || pending.has_value(); });
                if (shutting_down.load(std::memory_order_acquire)) return;
                request = std::move(*pending);
                pending.reset();
                request_in_progress = true;
            }

            emit(request, VoicePlaybackState::opening);
            auto outcome = play_one(request);
            if (cancelled(request.id) && outcome.state != VoicePlaybackState::finished) {
                outcome = {VoicePlaybackState::stopped, {}};
            }
            emit(request, outcome.state, std::move(outcome.message));
            {
                std::lock_guard<std::mutex> lock(mutex);
                request_in_progress = false;
                if (!pending && active_command.load(std::memory_order_acquire) != request.id) {
                    playback_state.store(VoicePlaybackState::stopped, std::memory_order_release);
                }
            }
        }
    }

    const std::string alsa_device;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    bool request_in_progress = false;
    VoiceRequestId next_id   = 1;
    std::atomic<VoiceRequestId> active_command{0};
    std::atomic<VoicePlaybackState> playback_state{VoicePlaybackState::idle};
    std::atomic<bool> shutting_down{false};
    std::thread worker;
};

VoicePlayer::VoicePlayer(std::string alsa_device) : impl_(std::make_unique<Impl>(std::move(alsa_device)))
{
}

VoicePlayer::~VoicePlayer() = default;

VoiceRequestId VoicePlayer::play_file(std::string path, Callback callback, std::string *error)
{
    return impl_->play(std::move(path), std::move(callback), error);
}

void VoicePlayer::stop() noexcept
{
    impl_->stop();
}

VoicePlaybackState VoicePlayer::state() const noexcept
{
    return impl_->playback_state.load(std::memory_order_acquire);
}

}  // namespace telegram::media
