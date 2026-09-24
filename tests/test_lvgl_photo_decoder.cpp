#include "pages/telegram/lvgl_photo_decoder.hpp"

#include <lvgl.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("telegram-jpeg-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory()
    {
        std::filesystem::create_directory(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_fixture(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(file.good());
}

std::vector<std::uint8_t> gray_jpeg(std::uint16_t width, std::uint16_t height)
{
    std::vector<std::uint8_t> result{0xff, 0xd8};
    const auto append_segment = [&result](std::uint8_t marker, const std::vector<std::uint8_t>& payload) {
        const auto length = payload.size() + 2U;
        result.insert(result.end(),
                      {0xff, marker, static_cast<std::uint8_t>(length >> 8U), static_cast<std::uint8_t>(length)});
        result.insert(result.end(), payload.begin(), payload.end());
    };
    std::vector<std::uint8_t> quantization(65, 1);
    quantization[0] = 0;
    append_segment(0xdb, quantization);
    append_segment(0xc0, {8, static_cast<std::uint8_t>(height >> 8U), static_cast<std::uint8_t>(height),
                          static_cast<std::uint8_t>(width >> 8U), static_cast<std::uint8_t>(width), 1, 1, 0x11, 0});
    std::vector<std::uint8_t> huffman(18, 0);
    huffman[1] = 1;
    append_segment(0xc4, huffman);
    huffman[0] = 0x10;
    append_segment(0xc4, huffman);
    append_segment(0xda, {1, 1, 0, 0, 63, 0});

    // Each constant-gray 8x8 block has a zero DC delta and an EOB, both encoded
    // as a single zero bit by these minimal baseline Huffman tables.
    const auto blocks = ((static_cast<std::size_t>(width) + 7U) / 8U) * ((height + 7U) / 8U);
    result.insert(result.end(), (blocks * 2U + 7U) / 8U, 0);
    if (blocks % 4U != 0) result.back() = static_cast<std::uint8_t>((1U << (8U - blocks % 4U * 2U)) - 1U);
    result.insert(result.end(), {0xff, 0xd9});
    return result;
}

std::array<std::uint8_t, 4> pixel_at(const telegram::media::DecodedImage& image, std::uint32_t x, std::uint32_t y)
{
    const auto* pixel = image.bgra.data() + y * image.stride + x * 4U;
    return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

void test_demo_image_complete_pixels(const std::string& path)
{
    const auto decoded = telegram::ui::decode_jpeg_with_lvgl(path);
    assert(decoded.ok());
    assert(decoded.source_path == path);
    const auto& image = *decoded.image;
    assert(image.width == 128 && image.height == 128);
    assert(image.stride == 128 * 4);
    assert(image.bgra.size() == 128 * 128 * 4);
    const auto white = pixel_at(image, 0, 0);
    assert(white[0] >= 250 && white[1] >= 250 && white[2] >= 250);
    const auto dark = pixel_at(image, 64, 99);
    assert(dark[0] <= 20 && dark[1] <= 20 && dark[2] <= 20);
    const auto peach = pixel_at(image, 64, 80);
    assert(peach[2] > 230 && peach[1] > 140 && peach[1] < 210 && peach[0] > 110 && peach[0] < 190);
    for (std::size_t offset = 3; offset < image.bgra.size(); offset += 4) assert(image.bgra[offset] == 255);

    const auto again = telegram::ui::decode_jpeg_with_lvgl(path);
    assert(again.ok());
    assert(again.image->bgra == image.bgra);

    const auto avatar = telegram::ui::decode_jpeg_with_lvgl(path, 120);
    assert(avatar.ok());
    assert(avatar.image->width == 120 && avatar.image->height == 120);
    assert(avatar.image->bgra.size() == 120 * 120 * 4);
    const auto avatar_center = pixel_at(*avatar.image, 60, 75);
    assert(avatar_center[2] > 230 && avatar_center[1] > 140 && avatar_center[1] < 210);
}

void test_downsampling_covers_every_output_pixel(const std::filesystem::path& directory)
{
    for (const auto dimensions : {std::array<std::uint16_t, 2>{1537, 769}, {25, 19}, {1, 2048}, {2048, 1}}) {
        const auto path = directory / "gray.jpg";
        write_fixture(path, gray_jpeg(dimensions[0], dimensions[1]));
        const auto decoded = telegram::ui::decode_jpeg_with_lvgl(path.string());
        assert(decoded.ok());
        const auto& image           = *decoded.image;
        const std::uint32_t longest = std::max(dimensions[0], dimensions[1]);
        const auto target           = std::min(720U, longest);
        assert(image.width == std::max(1U, dimensions[0] * target / longest));
        assert(image.height == std::max(1U, dimensions[1] * target / longest));
        assert(image.bgra.size() <= 4U * 1024U * 1024U);
        assert(image.stride == image.width * 4U);
        for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
            assert(image.bgra[offset] == 128 && image.bgra[offset + 1] == 128 && image.bgra[offset + 2] == 128);
            assert(image.bgra[offset + 3] == 255);
        }
    }
}

void test_rejects_oversized_and_incomplete_files(const std::filesystem::path& directory)
{
    const auto oversized = directory / "oversized.jpg";
    {
        std::ofstream file(oversized, std::ios::binary);
        file.seekp(static_cast<std::streamoff>(telegram::media::kMaxJpegEncodedBytes));
        file.put('\0');
    }
    auto result = telegram::ui::decode_jpeg_with_lvgl(oversized.string());
    assert(!result.ok() && !result.image);
    assert(result.error.find("8 MiB") != std::string::npos);

    const auto dimensions = directory / "dimensions.jpg";
    write_fixture(dimensions, gray_jpeg(2049, 8));
    result = telegram::ui::decode_jpeg_with_lvgl(dimensions.string());
    assert(!result.ok() && !result.image);
    assert(result.error.find("2048") != std::string::npos);

    const auto incomplete = directory / "incomplete.jpg";
    auto truncated        = gray_jpeg(1024, 1024);
    truncated.resize(truncated.size() / 2);
    write_fixture(incomplete, truncated);
    result = telegram::ui::decode_jpeg_with_lvgl(incomplete.string());
    assert(!result.ok() && !result.image);
    assert(result.error.find("complete") != std::string::npos);

    result = telegram::ui::decode_jpeg_with_lvgl((directory / "missing.jpg").string());
    assert(!result.ok() && !result.image && !result.error.empty());
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    lv_init();
    {
        TemporaryDirectory directory;
        test_demo_image_complete_pixels(argv[1]);
        test_downsampling_covers_every_output_pixel(directory.path);
        test_rejects_oversized_and_incomplete_files(directory.path);
    }
    lv_deinit();
}
