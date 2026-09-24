#include "services/media/image_decoder.hpp"

#include <cassert>
#include <string>

namespace {

void test_encoded_size_limit()
{
    std::string error = "stale";
    assert(telegram::media::validate_jpeg_encoded_size(0, &error));
    assert(error.empty());
    assert(telegram::media::validate_jpeg_encoded_size(telegram::media::kMaxJpegEncodedBytes, &error));
    assert(error.empty());
    assert(!telegram::media::validate_jpeg_encoded_size(telegram::media::kMaxJpegEncodedBytes + 1U, &error));
    assert(error == "JPEG encoded file exceeds the 8 MiB input limit");
}

void test_source_dimension_limits()
{
    std::string error;
    assert(telegram::media::validate_jpeg_source_dimensions(2048, 2048, &error));
    assert(error.empty());

    assert(!telegram::media::validate_jpeg_source_dimensions(0, 100, &error));
    assert(error == "JPEG has invalid source dimensions");
    assert(!telegram::media::validate_jpeg_source_dimensions(100, 0, &error));
    assert(error == "JPEG has invalid source dimensions");
    assert(!telegram::media::validate_jpeg_source_dimensions(2049, 1, &error));
    assert(error == "JPEG source width or height exceeds the 2048 pixel limit");
    assert(!telegram::media::validate_jpeg_source_dimensions(1, 2049, &error));
    assert(error == "JPEG source width or height exceeds the 2048 pixel limit");
    assert(!telegram::media::validate_jpeg_source_dimensions(2048, 2049, &error));
    assert(error == "JPEG source pixel count exceeds the 4 MiPixel limit");
}

}  // namespace

int main()
{
    test_encoded_size_limit();
    test_source_dimension_limits();
    return 0;
}
