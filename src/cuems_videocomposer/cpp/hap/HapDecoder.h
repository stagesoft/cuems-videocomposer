#ifndef VIDEOCOMPOSER_HAPDECODER_H
#define VIDEOCOMPOSER_HAPDECODER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "../video/GPUTextureFrameBuffer.h"

extern "C" {
#include <hap.h>
}

namespace videocomposer {

// Use HapVariant from GPUTextureFrameBuffer.h (no separate enum needed)

/**
 * Decoded texture information
 */
struct HapDecodedTexture {
    std::vector<uint8_t> data;      // DXT compressed data
    unsigned int format;             // HapTextureFormat enum value
    size_t size;                     // Size in bytes
    int width;                       // Texture width
    int height;                      // Texture height
};

/**
 * HapDecoder - C++ wrapper for Vidvox HAP library
 * 
 * Provides convenient methods to decode HAP frames from packets.
 * Handles all HAP variants: HAP, HAP Q, HAP Alpha, HAP Q Alpha.
 */
class HapDecoder {
public:
    HapDecoder();
    ~HapDecoder();

    /**
     * Decode a HAP frame from packet data
     * @param packetData Raw HAP packet data
     * @param packetSize Size of packet in bytes
     * @param width Frame width (for validation)
     * @param height Frame height (for validation)
     * @param textures Output vector to receive decoded textures (1 or 2)
     * @return true on success, false on error
     */
    bool decode(const uint8_t* packetData, size_t packetSize,
                int width, int height,
                std::vector<HapDecodedTexture>& textures);

    /**
     * Get the HAP variant from packet data (without decoding)
     * @param packetData Raw HAP packet data
     * @param packetSize Size of packet in bytes
     * @return HapVariant enum value
     */
    static HapVariant getVariant(const uint8_t* packetData, size_t packetSize);

    /**
     * Get the number of textures in a HAP frame
     * @param packetData Raw HAP packet data
     * @param packetSize Size of packet in bytes
     * @return Number of textures (1 or 2), or 0 on error
     */
    static unsigned int getTextureCount(const uint8_t* packetData, size_t packetSize);

    /**
     * Get error message from last decode failure
     * @return Error message string
     */
    const std::string& getLastError() const { return lastError_; }

private:
    /**
     * Calculate expected DXT buffer size based on dimensions and format
     */
    static size_t calculateDXTSize(int width, int height, unsigned int format);

    /**
     * Decode callback for multithreaded decoding (not used for now)
     */
    static void decodeCallback(HapDecodeWorkFunction function, void *p, 
                               unsigned int count, void *info);

    std::string lastError_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HAPDECODER_H

