#ifndef LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_LVGL_PHOTO_DECODER_HPP
#define LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_LVGL_PHOTO_DECODER_HPP

#include "services/media/image_decoder.hpp"

namespace telegram::ui {

// Call on the LVGL thread. The result owns its pixels independently of LVGL.
[[nodiscard]] media::ImageDecodeResult decode_jpeg_with_lvgl(const std::string& path, std::uint32_t max_dimension = 720);
[[nodiscard]] media::ImageDecodeResult decode_png_avatar_with_lvgl(const std::string& path);

}  // namespace telegram::ui

#endif
