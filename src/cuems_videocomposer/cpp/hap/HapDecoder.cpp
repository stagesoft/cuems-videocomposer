#include "HapDecoder.h"
#include "../utils/Logger.h"
#include <cstring>
#include <sstream>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace videocomposer {

HapDecoder::HapDecoder() {
}

HapDecoder::~HapDecoder() {
}

bool HapDecoder::decode(const uint8_t* packetData, size_t packetSize,
                        int width, int height,
                        std::vector<HapDecodedTexture>& textures) {
    if (!packetData || packetSize == 0) {
        lastError_ = "Invalid packet data";
        return false;
    }

    // Clear output
    textures.clear();

    // Get texture count
    unsigned int textureCount = 0;
    unsigned int result = HapGetFrameTextureCount(packetData, packetSize, &textureCount);
    if (result != HapResult_No_Error) {
        std::ostringstream oss;
        oss << "HapGetFrameTextureCount failed with code " << result;
        lastError_ = oss.str();
        return false;
    }

    if (textureCount == 0 || textureCount > 2) {
        std::ostringstream oss;
        oss << "Invalid texture count: " << textureCount;
        lastError_ = oss.str();
        return false;
    }

    LOG_VERBOSE << "HAP frame has " << textureCount << " texture(s)";

    // Decode each texture
    for (unsigned int i = 0; i < textureCount; i++) {
        HapDecodedTexture texture;

        // Get texture format
        unsigned int textureFormat = 0;
        result = HapGetFrameTextureFormat(packetData, packetSize, i, &textureFormat);
        if (result != HapResult_No_Error) {
            std::ostringstream oss;
            oss << "HapGetFrameTextureFormat failed for texture " << i << " with code " << result;
            lastError_ = oss.str();
            return false;
        }

        texture.format = textureFormat;
        texture.width = width;
        texture.height = height;

        // Calculate expected DXT size
        size_t dxtSize = calculateDXTSize(width, height, textureFormat);
        if (dxtSize == 0) {
            std::ostringstream oss;
            oss << "Unknown texture format: 0x" << std::hex << textureFormat;
            lastError_ = oss.str();
            return false;
        }

        // Allocate buffer for decompressed DXT data
        texture.data.resize(dxtSize);
        texture.size = dxtSize;

        // Decode texture
        unsigned long bytesUsed = 0;
        result = HapDecode(packetData, packetSize, i,
                          decodeCallback, nullptr,  // Single-threaded decode callback
                          texture.data.data(), dxtSize,
                          &bytesUsed,
                          &textureFormat);

        if (result != HapResult_No_Error) {
            std::ostringstream oss;
            oss << "HapDecode failed for texture " << i << " with code " << result;
            lastError_ = oss.str();
            return false;
        }

        // Verify bytes used
        if (bytesUsed != dxtSize) {
            LOG_VERBOSE << "HAP decode: expected " << dxtSize << " bytes, got " << bytesUsed;
            texture.size = bytesUsed;
            texture.data.resize(bytesUsed);
        }

        LOG_VERBOSE << "Decoded HAP texture " << i << ": format=0x" << std::hex << textureFormat
                   << std::dec << ", size=" << bytesUsed << " bytes";

        textures.push_back(std::move(texture));
    }

    return true;
}

HapVariant HapDecoder::getVariant(const uint8_t* packetData, size_t packetSize) {
    if (!packetData || packetSize == 0) {
        return HapVariant::NONE;
    }

    // Get texture count
    unsigned int textureCount = 0;
    if (HapGetFrameTextureCount(packetData, packetSize, &textureCount) != HapResult_No_Error) {
        return HapVariant::NONE;
    }

    if (textureCount == 0 || textureCount > 2) {
        return HapVariant::NONE;
    }

    // Get first texture format
    unsigned int format0 = 0;
    if (HapGetFrameTextureFormat(packetData, packetSize, 0, &format0) != HapResult_No_Error) {
        return HapVariant::NONE;
    }

    // Single texture formats
    if (textureCount == 1) {
        switch (format0) {
            case HapTextureFormat_RGB_DXT1:
                return HapVariant::HAP;
            case HapTextureFormat_RGBA_DXT5:
                return HapVariant::HAP_ALPHA;
            case HapTextureFormat_YCoCg_DXT5:
                return HapVariant::HAP_Q;
            case HapTextureFormat_RGBA_BPTC_UNORM:
                return HapVariant::HAP_R;  // HAP R (BPTC/BC7 - best quality + alpha) - UNTESTED
            default:
                return HapVariant::NONE;
        }
    }

    // Dual texture (HAP Q Alpha)
    if (textureCount == 2) {
        // First should be YCoCg, second should be alpha
        if (format0 == HapTextureFormat_YCoCg_DXT5) {
            unsigned int format1 = 0;
            if (HapGetFrameTextureFormat(packetData, packetSize, 1, &format1) == HapResult_No_Error) {
                if (format1 == HapTextureFormat_A_RGTC1) {
                    return HapVariant::HAP_Q_ALPHA;
                }
            }
        }
    }

    return HapVariant::NONE;
}

unsigned int HapDecoder::getTextureCount(const uint8_t* packetData, size_t packetSize) {
    if (!packetData || packetSize == 0) {
        return 0;
    }

    unsigned int textureCount = 0;
    if (HapGetFrameTextureCount(packetData, packetSize, &textureCount) != HapResult_No_Error) {
        return 0;
    }

    return textureCount;
}

size_t HapDecoder::calculateDXTSize(int width, int height, unsigned int format) {
    // DXT formats compress 4x4 pixel blocks
    int blockWidth = (width + 3) / 4;
    int blockHeight = (height + 3) / 4;
    int blockCount = blockWidth * blockHeight;

    switch (format) {
        case HapTextureFormat_RGB_DXT1:
            // DXT1: 8 bytes per 4x4 block
            return blockCount * 8;

        case HapTextureFormat_RGBA_DXT5:
        case HapTextureFormat_YCoCg_DXT5:
            // DXT5: 16 bytes per 4x4 block
            return blockCount * 16;

        case HapTextureFormat_A_RGTC1:
            // BC4/RGTC1: 8 bytes per 4x4 block (single channel)
            return blockCount * 8;

        case HapTextureFormat_RGBA_BPTC_UNORM:
        case HapTextureFormat_RGB_BPTC_SIGNED_FLOAT:
        case HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT:
            // BC7/BPTC: 16 bytes per 4x4 block
            // NOTE: HAP R (RGBA_BPTC_UNORM) support is UNTESTED
            return blockCount * 16;

        default:
            LOG_WARNING << "Unknown HAP texture format: 0x" << std::hex << format;
            return 0;
    }
}

void HapDecoder::decodeCallback(HapDecodeWorkFunction function, void *p,
                                unsigned int count, void *info) {
    // Parallel Snappy chunk decompression using OpenMP
    // This provides ~0.3-0.6ms savings per frame for 1080p HAP
    // 
    // TODO: For larger performance gains with multi-layer HAP playback,
    // implement a shared thread pool for layer-parallel decoding in LayerManager.
    // Layer-parallel decoding can provide 6-7x speedup for 10+ simultaneous layers
    // by decoding all layer frames in parallel before rendering.
    // See: LayerManager::update() - could submit decode jobs to thread pool
    
#ifdef _OPENMP
    #pragma omp parallel for schedule(dynamic)
    for (unsigned int i = 0; i < count; i++) {
        function(p, i);
    }
#else
    // Fallback: sequential execution
    for (unsigned int i = 0; i < count; i++) {
        function(p, i);
    }
#endif
}

} // namespace videocomposer

