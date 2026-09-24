#include "services/media/image_decoder.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#if defined(TELEGRAM_HAS_JPEG)
#include <csetjmp>
#include <jpeglib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace telegram::media {

bool validate_jpeg_encoded_size(std::uint64_t encoded_bytes, std::string *error)
{
    if (encoded_bytes > kMaxJpegEncodedBytes) {
        if (error) *error = "JPEG encoded file exceeds the 8 MiB input limit";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool validate_jpeg_source_dimensions(std::uint32_t width, std::uint32_t height, std::string *error)
{
    if (width == 0 || height == 0) {
        if (error) *error = "JPEG has invalid source dimensions";
        return false;
    }
    if (static_cast<std::uint64_t>(width) * height > kMaxJpegSourcePixels) {
        if (error) *error = "JPEG source pixel count exceeds the 4 MiPixel limit";
        return false;
    }
    if (width > kMaxJpegSourceDimension || height > kMaxJpegSourceDimension) {
        if (error) *error = "JPEG source width or height exceeds the 2048 pixel limit";
        return false;
    }
    if (error) error->clear();
    return true;
}

namespace {

[[maybe_unused]] constexpr const char *kJpegUnavailable =
    "JPEG decoding is unavailable: build with TELEGRAM_HAS_JPEG and libjpeg";

void set_error(std::string *target, const char *message)
{
    if (target) *target = message;
}

#if defined(TELEGRAM_HAS_JPEG)

struct JpegErrorManager {
    jpeg_error_mgr base{};
    std::jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
};

struct JpegDimensions {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
};

void jpeg_error_exit(j_common_ptr common)
{
    auto *error = reinterpret_cast<JpegErrorManager *>(common->err);
    error->base.format_message(common, error->message);
    std::longjmp(error->jump, 1);
}

bool validate_open_jpeg_size(FILE *file, char *error_text, std::size_t error_size)
{
    struct stat file_info{};
    if (::fstat(::fileno(file), &file_info) != 0 || file_info.st_size < 0) {
        std::snprintf(error_text, error_size, "cannot inspect JPEG encoded file size");
        return false;
    }

    std::string validation_error;
    if (!validate_jpeg_encoded_size(static_cast<std::uint64_t>(file_info.st_size), &validation_error)) {
        std::snprintf(error_text, error_size, "%s", validation_error.c_str());
        return false;
    }
    return true;
}

bool read_jpeg_dimensions(const char *path, JpegDimensions *dimensions, char *error_text, std::size_t error_size)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) {
        std::snprintf(error_text, error_size, "cannot open JPEG file: %s", path);
        return false;
    }
    if (!validate_open_jpeg_size(file, error_text, error_size)) {
        std::fclose(file);
        return false;
    }

    jpeg_decompress_struct decoder{};
    JpegErrorManager error{};
    decoder.err               = jpeg_std_error(&error.base);
    error.base.error_exit     = jpeg_error_exit;
    volatile bool initialized = false;

    if (setjmp(error.jump) != 0) {
        if (initialized) jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        std::snprintf(error_text, error_size, "invalid JPEG: %s", error.message);
        return false;
    }

    jpeg_create_decompress(&decoder);
    initialized = true;
    jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE);
    dimensions->width  = decoder.image_width;
    dimensions->height = decoder.image_height;
    jpeg_destroy_decompress(&decoder);
    std::fclose(file);

    std::string validation_error;
    if (!validate_jpeg_source_dimensions(dimensions->width, dimensions->height, &validation_error)) {
        std::snprintf(error_text, error_size, "%s", validation_error.c_str());
        return false;
    }
    return true;
}

bool scaled_dimensions(const JpegDimensions &source, const ImageDecodeOptions &options, JpegDimensions *target,
                       std::string *error)
{
    std::uint64_t numerator   = 1;
    std::uint64_t denominator = 1;

    if (options.max_width != 0 && source.width > options.max_width) {
        numerator   = options.max_width;
        denominator = source.width;
    }
    if (options.max_height != 0 && source.height > options.max_height &&
        static_cast<std::uint64_t>(options.max_height) * denominator < numerator * source.height) {
        numerator   = options.max_height;
        denominator = source.height;
    }

    target->width = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(source.width) * numerator / denominator));
    target->height = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(source.height) * numerator / denominator));

    const auto pixel_count = static_cast<std::uint64_t>(target->width) * target->height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U || pixel_count * 4U > options.max_output_bytes) {
        if (error) *error = "decoded JPEG exceeds the configured output limit";
        return false;
    }
    return true;
}

unsigned int choose_scale_denominator(const JpegDimensions &source, const JpegDimensions &target)
{
    unsigned int selected = 1;
    for (const unsigned int candidate : {2U, 4U, 8U}) {
        const auto scaled_width  = (source.width + candidate - 1U) / candidate;
        const auto scaled_height = (source.height + candidate - 1U) / candidate;
        if (scaled_width >= target.width && scaled_height >= target.height) selected = candidate;
    }
    return selected;
}

bool decode_jpeg_pixels(const char *path, const JpegDimensions &expected, const JpegDimensions &target,
                        DecodedImage *image, char *error_text, std::size_t error_size)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) {
        std::snprintf(error_text, error_size, "cannot open JPEG file: %s", path);
        return false;
    }
    if (!validate_open_jpeg_size(file, error_text, error_size)) {
        std::fclose(file);
        return false;
    }

    jpeg_decompress_struct decoder{};
    JpegErrorManager error{};
    decoder.err                        = jpeg_std_error(&error.base);
    error.base.error_exit              = jpeg_error_exit;
    volatile bool initialized          = false;
    unsigned char *volatile row_buffer = nullptr;

    if (setjmp(error.jump) != 0) {
        std::free(const_cast<unsigned char *>(row_buffer));
        if (initialized) jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        std::snprintf(error_text, error_size, "JPEG decode failed: %s", error.message);
        return false;
    }

    jpeg_create_decompress(&decoder);
    initialized = true;
    jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE);
    bool dimensions_valid = false;
    {
        std::string validation_error;
        dimensions_valid =
            validate_jpeg_source_dimensions(decoder.image_width, decoder.image_height, &validation_error);
        if (!dimensions_valid) std::snprintf(error_text, error_size, "%s", validation_error.c_str());
    }
    if (!dimensions_valid) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        return false;
    }
    if (decoder.image_width != expected.width || decoder.image_height != expected.height) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        std::snprintf(error_text, error_size, "JPEG changed while it was being decoded");
        return false;
    }

    decoder.out_color_space = JCS_RGB;
    decoder.scale_num       = 1;
    decoder.scale_denom     = choose_scale_denominator(expected, target);
    jpeg_calc_output_dimensions(&decoder);
    while ((decoder.output_width < target.width || decoder.output_height < target.height) && decoder.scale_denom > 1) {
        decoder.scale_denom /= 2;
        jpeg_calc_output_dimensions(&decoder);
    }

    const std::size_t row_bytes = static_cast<std::size_t>(decoder.output_width) * 3U;
    row_buffer                  = static_cast<unsigned char *>(std::malloc(row_bytes));
    if (!row_buffer) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        std::snprintf(error_text, error_size, "not enough memory for JPEG scanline");
        return false;
    }

    jpeg_start_decompress(&decoder);
    if (decoder.output_components != 3 || decoder.output_width < target.width ||
        decoder.output_height < target.height) {
        std::free(const_cast<unsigned char *>(row_buffer));
        row_buffer = nullptr;
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        std::snprintf(error_text, error_size, "unsupported JPEG color format");
        return false;
    }

    std::uint32_t target_y = 0;
    while (decoder.output_scanline < decoder.output_height) {
        const auto source_y = decoder.output_scanline;
        auto *row           = const_cast<unsigned char *>(row_buffer);
        JSAMPROW rows[]     = {row};
        if (jpeg_read_scanlines(&decoder, rows, 1) != 1) {
            std::free(row);
            row_buffer = nullptr;
            jpeg_destroy_decompress(&decoder);
            std::fclose(file);
            std::snprintf(error_text, error_size, "JPEG decoder returned an incomplete scanline");
            return false;
        }

        while (target_y < target.height) {
            const auto mapped_y =
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(target_y) * 2U + 1U) * decoder.output_height /
                                           (static_cast<std::uint64_t>(target.height) * 2U));
            if (mapped_y != source_y) break;

            auto *destination = image->bgra.data() + static_cast<std::size_t>(target_y) * image->stride;
            for (std::uint32_t target_x = 0; target_x < target.width; ++target_x) {
                const auto source_x =
                    static_cast<std::uint32_t>((static_cast<std::uint64_t>(target_x) * 2U + 1U) * decoder.output_width /
                                               (static_cast<std::uint64_t>(target.width) * 2U));
                const auto source_offset            = static_cast<std::size_t>(source_x) * 3U;
                const auto destination_offset       = static_cast<std::size_t>(target_x) * 4U;
                destination[destination_offset]     = row[source_offset + 2];
                destination[destination_offset + 1] = row[source_offset + 1];
                destination[destination_offset + 2] = row[source_offset];
                destination[destination_offset + 3] = 0xff;
            }
            ++target_y;
        }
    }

    std::free(const_cast<unsigned char *>(row_buffer));
    row_buffer = nullptr;
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    std::fclose(file);

    if (target_y != target.height) {
        std::snprintf(error_text, error_size, "JPEG decoder returned incomplete image data");
        return false;
    }
    return true;
}

std::shared_ptr<const DecodedImage> decode_jpeg(const std::string &path, const ImageDecodeOptions &options,
                                                std::string *error)
{
    char error_text[256]{};
    JpegDimensions source{};
    if (!read_jpeg_dimensions(path.c_str(), &source, error_text, sizeof(error_text))) {
        *error = error_text;
        return nullptr;
    }

    JpegDimensions target{};
    if (!scaled_dimensions(source, options, &target, error)) return nullptr;

    auto image    = std::make_shared<DecodedImage>();
    image->width  = target.width;
    image->height = target.height;
    image->stride = static_cast<std::size_t>(target.width) * 4U;
    image->bgra.resize(image->stride * target.height);
    if (!decode_jpeg_pixels(path.c_str(), source, target, image.get(), error_text, sizeof(error_text))) {
        *error = error_text;
        return nullptr;
    }
    return image;
}

#endif

std::shared_ptr<const DecodedImage> decode_file(const std::string &path, const ImageDecodeOptions &options,
                                                std::string *error)
{
#if defined(TELEGRAM_HAS_JPEG)
    return decode_jpeg(path, options, error);
#else
    (void)path;
    (void)options;
    *error = kJpegUnavailable;
    return nullptr;
#endif
}

}  // namespace

struct ImageDecoder::Impl {
    struct Request {
        ImageRequestId id = 0;
        std::string path;
        ImageDecodeOptions options;
        Callback callback;
    };

    explicit Impl(std::size_t request_limit) : limit(std::max<std::size_t>(1, request_limit)), worker([this] { run(); })
    {
    }

    ~Impl()
    {
        stop();
    }

    bool submit(std::string path, ImageDecodeOptions options, Callback callback, ImageRequestId *request_id,
                std::string *error)
    {
        if (path.empty()) {
            set_error(error, "image path is empty");
            return false;
        }
        if (!callback) {
            set_error(error, "image callback is empty");
            return false;
        }
        if (options.max_output_bytes < 4U) {
            set_error(error, "image output limit is too small");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            set_error(error, "image decoder has stopped");
            return false;
        }
        if (outstanding_count >= limit) {
            set_error(error, "image decoder queue is full");
            return false;
        }

        const auto id = next_id++;
        requests.push_back(Request{id, std::move(path), options, std::move(callback)});
        ++outstanding_count;
        if (request_id) *request_id = id;
        if (error) error->clear();
        condition.notify_one();
        return true;
    }

    void stop() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping) {
                stopping = true;
                outstanding_count -= requests.size();
                requests.clear();
            }
        }
        condition.notify_all();
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    }

    [[nodiscard]] std::size_t outstanding() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return outstanding_count;
    }

    void run() noexcept
    {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !requests.empty(); });
                if (stopping && requests.empty()) return;
                request = std::move(requests.front());
                requests.pop_front();
            }

            ImageDecodeResult result;
            result.request_id  = request.id;
            result.source_path = request.path;
            try {
                result.image = decode_file(request.path, request.options, &result.error);
            } catch (const std::exception &exception) {
                result.error = std::string("image decode failed: ") + exception.what();
            } catch (...) {
                result.error = "image decode failed with an unknown error";
            }

            bool deliver = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                --outstanding_count;
                deliver = !stopping;
            }
            if (deliver) {
                try {
                    request.callback(std::move(result));
                } catch (...) {
                    // Client callbacks must not terminate the long-lived worker.
                }
            }
        }
    }

    const std::size_t limit;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Request> requests;
    std::size_t outstanding_count = 0;
    ImageRequestId next_id        = 1;
    bool stopping                 = false;
    std::thread worker;
};

ImageDecoder::ImageDecoder(std::size_t max_requests) : impl_(std::make_unique<Impl>(max_requests))
{
}

ImageDecoder::~ImageDecoder() = default;

bool ImageDecoder::submit_file(std::string path, ImageDecodeOptions options, Callback callback,
                               ImageRequestId *request_id, std::string *error)
{
    return impl_->submit(std::move(path), options, std::move(callback), request_id, error);
}

void ImageDecoder::stop() noexcept
{
    impl_->stop();
}

std::size_t ImageDecoder::outstanding() const noexcept
{
    return impl_->outstanding();
}

}  // namespace telegram::media
