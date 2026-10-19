#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rfv::win {

struct DecodedImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, straight alpha
};

// Decodes PNG/JPEG/BMP/etc. with Windows Imaging Component. The calling thread must have
// initialised COM. Returns false (with `error` set) on failure.
bool decodeImage(const std::vector<uint8_t>& encoded, DecodedImage& out, std::string& error);

} // namespace rfv::win
