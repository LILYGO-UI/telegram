#ifndef LILYGO_UI_TELEGRAM_SERVICES_MEDIA_IMAGE_DECODER_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_MEDIA_IMAGE_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace telegram::media {

using ImageRequestId = std::uint64_t;

inline constexpr std::uint64_t kMaxJpegEncodedBytes    = 8U * 1024U * 1024U;
inline constexpr std::uint32_t kMaxJpegSourceDimension = 2048U;
inline constexpr std::uint64_t kMaxJpegSourcePixels    = 4U * 1024U * 1024U;

// These checks are independent of libjpeg so the resource policy remains
// testable in builds that use LVGL's JPEG fallback.
[[nodiscard]] bool validate_jpeg_encoded_size(std::uint64_t encoded_bytes, std::string *error = nullptr);
[[nodiscard]] bool validate_jpeg_source_dimensions(std::uint32_t width, std::uint32_t height,
                                                   std::string *error = nullptr);

struct ImageDecodeOptions {
    std::uint32_t max_width      = 1280;
    std::uint32_t max_height     = 1280;
    std::size_t max_output_bytes = 16U * 1024U * 1024U;
};

struct DecodedImage {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::size_t stride   = 0;
    // Byte order is B, G, R, A so the buffer can be used directly as LVGL's
    // LV_COLOR_FORMAT_ARGB8888 on little-endian ARM without another copy.
    std::vector<std::uint8_t> bgra;
};

struct ImageDecodeResult {
    ImageRequestId request_id = 0;
    std::string source_path;
    std::shared_ptr<const DecodedImage> image;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return image != nullptr && error.empty();
    }
};

class ImageDecoder {
public:
    using Callback = std::function<void(ImageDecodeResult)>;

    // max_requests includes the request currently being decoded.
    explicit ImageDecoder(std::size_t max_requests = 4);
    ~ImageDecoder();

    ImageDecoder(const ImageDecoder &)            = delete;
    ImageDecoder &operator=(const ImageDecoder &) = delete;
    ImageDecoder(ImageDecoder &&)                 = delete;
    ImageDecoder &operator=(ImageDecoder &&)      = delete;

    // The callback runs on the decoder worker thread and must marshal UI work
    // to the LVGL thread. It must not destroy this decoder from inside the
    // callback. Returns false immediately if validation fails, the bounded
    // queue is full, or the decoder has stopped.
    bool submit_file(std::string path, ImageDecodeOptions options, Callback callback,
                     ImageRequestId *request_id = nullptr, std::string *error = nullptr);

    // Pending callbacks are discarded. A callback already in progress is
    // allowed to finish before this call returns.
    void stop() noexcept;

    [[nodiscard]] std::size_t outstanding() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace telegram::media

#endif
