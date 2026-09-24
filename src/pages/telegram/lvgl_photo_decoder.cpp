#include "pages/telegram/lvgl_photo_decoder.hpp"

#include <lvgl.h>
#include <lvgl_private.h>

#include <algorithm>
#include <filesystem>
#include <new>

namespace telegram::ui {
namespace {

struct DecoderSession {
    lv_image_decoder_dsc_t descriptor{};
    bool opened = false;

    ~DecoderSession()
    {
        if (opened) lv_image_decoder_close(&descriptor);
    }
};

#if LV_USE_TJPGD
constexpr std::uint32_t kMaxOutputDimension = 720;
constexpr std::size_t kMaxOutputBytes       = 4U * 1024U * 1024U;

media::ImageDecodeResult decode_bounded_jpeg(const std::string& path, std::uint32_t max_dimension)
{
    media::ImageDecodeResult result;
    result.source_path = path;
    std::error_code filesystem_error;
    const auto encoded_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        result.error = "JPEG file can't be read";
        return result;
    }
    if (!media::validate_jpeg_encoded_size(encoded_size, &result.error)) return result;

    const std::string source = "A:" + path;
    lv_image_header_t header{};
    if (lv_image_decoder_get_info(source.c_str(), &header) != LV_RESULT_OK || header.cf != LV_COLOR_FORMAT_RAW) {
        result.error = "The built-in decoder couldn't read this baseline JPEG";
        return result;
    }
    if (!media::validate_jpeg_source_dimensions(header.w, header.h, &result.error)) return result;

    DecoderSession session;
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    if (lv_image_decoder_open(&session.descriptor, source.c_str(), &args) != LV_RESULT_OK) {
        result.error = "The built-in decoder couldn't open this baseline JPEG";
        return result;
    }
    session.opened             = true;
    auto& decoder              = session.descriptor;
    const std::uint32_t width  = decoder.header.w;
    const std::uint32_t height = decoder.header.h;
    if (!media::validate_jpeg_source_dimensions(width, height, &result.error)) return result;
    if (decoder.header.cf != LV_COLOR_FORMAT_RGB888) {
        result.error = "JPEG decoder returned an unsupported pixel format";
        return result;
    }

    const auto longest        = std::max(width, height);
    const auto scaled_longest = std::min(longest, std::clamp(max_dimension, 1U, kMaxOutputDimension));
    auto output               = std::make_shared<media::DecodedImage>();
    output->width             = std::max(1U, width * scaled_longest / longest);
    output->height            = std::max(1U, height * scaled_longest / longest);
    output->stride            = output->width * 4U;
    const auto output_bytes   = output->stride * output->height;
    if (output_bytes > kMaxOutputBytes) {
        result.error = "Decoded JPEG exceeds the 4 MiB output limit";
        return result;
    }
    output->bgra.resize(output_bytes);

    const lv_area_t full{0, 0, static_cast<std::int32_t>(width) - 1, static_cast<std::int32_t>(height) - 1};
    lv_area_t tile{LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    const bool complete_buffer      = decoder.decoded != nullptr;
    const std::size_t source_pixels = static_cast<std::size_t>(width) * height;
    std::size_t decoded_pixels      = 0;

    // TJPGD's MCU buffers cannot be transformed independently. Assemble into
    // the bounded final image while sampling, without allocating the full source.
    while (decoded_pixels < source_pixels) {
        if (complete_buffer) {
            tile = full;
        } else if (lv_image_decoder_get_area(&decoder, &full, &tile) != LV_RESULT_OK) {
            result.error = "JPEG decoding stopped before the image was complete";
            return result;
        }
        const auto* decoded = decoder.decoded;
        if (!decoded || !decoded->data || decoded->header.cf != LV_COLOR_FORMAT_RGB888 || tile.x1 < 0 || tile.y1 < 0 ||
            tile.x2 < tile.x1 || tile.y2 < tile.y1 || tile.x2 >= static_cast<std::int32_t>(width) ||
            tile.y2 >= static_cast<std::int32_t>(height)) {
            result.error = "JPEG decoder returned an invalid pixel area";
            return result;
        }
        const auto tile_width  = static_cast<std::size_t>(lv_area_get_width(&tile));
        const auto tile_height = static_cast<std::size_t>(lv_area_get_height(&tile));
        const auto tile_stride = static_cast<std::size_t>(decoded->header.stride);
        if (decoded->header.w != tile_width || decoded->header.h != tile_height || tile_stride < tile_width * 3U ||
            decoded->data_size < (tile_height - 1U) * tile_stride + tile_width * 3U ||
            tile_width * tile_height > source_pixels - decoded_pixels) {
            result.error = "JPEG decoder returned an invalid pixel buffer";
            return result;
        }
        decoded_pixels += tile_width * tile_height;

        const auto x_begin = (static_cast<std::uint32_t>(tile.x1) * output->width + width - 1U) / width;
        const auto x_end   = (static_cast<std::uint32_t>(tile.x2 + 1) * output->width + width - 1U) / width;
        const auto y_begin = (static_cast<std::uint32_t>(tile.y1) * output->height + height - 1U) / height;
        const auto y_end   = (static_cast<std::uint32_t>(tile.y2 + 1) * output->height + height - 1U) / height;
        for (auto y = y_begin; y < y_end; ++y) {
            const auto source_y = y * height / output->height - static_cast<std::uint32_t>(tile.y1);
            for (auto x = x_begin; x < x_end; ++x) {
                const auto source_x = x * width / output->width - static_cast<std::uint32_t>(tile.x1);
                const auto* pixel   = decoded->data + source_y * tile_stride + source_x * 3U;
                auto* destination   = output->bgra.data() + y * output->stride + x * 4U;
                // LVGL RGB888 stores its channels in B, G, R memory order.
                destination[0] = pixel[0];
                destination[1] = pixel[1];
                destination[2] = pixel[2];
                destination[3] = 255;
            }
        }
    }
    result.image = std::move(output);
    return result;
}
#endif

}  // namespace

media::ImageDecodeResult decode_jpeg_with_lvgl(const std::string& path, std::uint32_t max_dimension)
{
#if LV_USE_TJPGD
    try {
        return decode_bounded_jpeg(path, max_dimension);
    } catch (const std::bad_alloc&) {
        media::ImageDecodeResult result;
        result.source_path = path;
        result.error       = "Not enough memory to decode JPEG";
        return result;
    }
#else
    (void)max_dimension;
    media::ImageDecodeResult result;
    result.source_path = path;
    result.error       = "The built-in JPEG decoder is unavailable";
    return result;
#endif
}

media::ImageDecodeResult decode_png_avatar_with_lvgl(const std::string& path)
{
    media::ImageDecodeResult result;
    result.source_path = path;
    try {
        const std::string source = "A:" + path;
        lv_image_header_t header{};
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > 8U * 1024U * 1024U ||
            lv_image_decoder_get_info(source.c_str(), &header) != LV_RESULT_OK ||
            header.w == 0 || header.h == 0 || header.w > 720 || header.h > 720 ||
            header.cf != LV_COLOR_FORMAT_ARGB8888) {
            result.error = "Avatar PNG exceeds decoding limits or has an unsupported format";
            return result;
        }
        DecoderSession session;
        lv_image_decoder_args_t args{};
        args.no_cache = true;
        if (lv_image_decoder_open(&session.descriptor, source.c_str(), &args) != LV_RESULT_OK) {
            result.error = "Avatar PNG could not be decoded";
            return result;
        }
        session.opened = true;
        const auto* decoded = session.descriptor.decoded;
        if (!decoded || !decoded->data || decoded->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
            decoded->header.w != header.w || decoded->header.h != header.h ||
            decoded->header.stride < header.w * 4U ||
            decoded->data_size < (header.h - 1U) * decoded->header.stride + header.w * 4U) {
            result.error = "Avatar PNG returned an invalid pixel buffer";
            return result;
        }
        auto output = std::make_shared<media::DecodedImage>();
        const auto longest = std::max(header.w, header.h);
        const auto target = std::min<std::uint32_t>(longest, 120);
        output->width = std::max(1U, header.w * target / longest);
        output->height = std::max(1U, header.h * target / longest);
        output->stride = output->width * 4U;
        output->bgra.resize(output->stride * output->height);
        for (std::uint32_t y = 0; y < output->height; ++y) {
            for (std::uint32_t x = 0; x < output->width; ++x) {
                const auto* pixel = decoded->data + (y * header.h / output->height) * decoded->header.stride +
                                    (x * header.w / output->width) * 4U;
                std::copy_n(pixel, 4, output->bgra.data() + y * output->stride + x * 4U);
            }
        }
        result.image = std::move(output);
    } catch (const std::bad_alloc&) {
        result.error = "Not enough memory to decode avatar";
    }
    return result;
}

}  // namespace telegram::ui
