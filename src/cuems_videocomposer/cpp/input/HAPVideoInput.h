#ifndef VIDEOCOMPOSER_HAPVIDEOINPUT_H
#define VIDEOCOMPOSER_HAPVIDEOINPUT_H

#include "InputSource.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <cuems_mediadecoder/VideoDecoder.h>
#include <string>
#include <memory>
#include <cstdint>

#ifdef ENABLE_HAP_DIRECT
#include "../hap/HapDecoder.h"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace videocomposer {

/**
 * HAPVideoInput - Specialized InputSource for HAP codec
 * 
 * HAP (Hardware Accelerated Performance) is a codec designed for VJ/live performance.
 * Key features:
 * - Decodes directly to OpenGL textures (DXT1/DXT5 compressed)
 * - Zero-copy: frames stay on GPU, no CPU→GPU transfer
 * - Optimized for multiple concurrent streams
 * 
 * This class provides HAP-specific decoding that outputs directly to GPU textures.
 */
class HAPVideoInput : public InputSource {
public:
    HAPVideoInput();
    virtual ~HAPVideoInput();

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override;
    bool seek(int64_t frameNumber) override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override;
    DecodeBackend getOptimalBackend() const override;

    // HAP-specific methods
    /**
     * Read a frame directly to GPU texture (HAP-specific)
     * @param frameNumber Frame number to read
     * @param textureBuffer GPUTextureFrameBuffer to store the decoded texture
     * @return true on success, false on failure
     */
    bool readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer);

    /**
     * Get HAP variant (HAP, HAP Q, HAP Alpha)
     * @return HAP variant codec type
     */
    CodecType getHAPVariant() const;


private:
    enum class HAPVariant {
        HAP,        // Standard HAP (DXT1)
        HAP_Q,      // HAP Q (DXT5, higher quality)
        HAP_ALPHA   // HAP Alpha (DXT5, with alpha channel)
    };

    bool initializeFFmpeg();
    bool openCodec();
    bool seekToFrame(int64_t frameNumber);
    HAPVariant detectHAPVariant();
    void refineHAPVariantFromFrame(AVFrame* frame);
    int64_t parsePTSFromFrame(AVFrame* frame);
    void cleanup();
    
#ifdef ENABLE_HAP_DIRECT
    // Direct HAP decoding methods (Vidvox SDK)
    bool decodeHapDirectToTexture(AVPacket* packet, GPUTextureFrameBuffer& textureBuffer);
    bool readRawPacket(int64_t frameNumber, AVPacket* packet);
#endif
    
    // FFmpeg fallback methods
    bool decodeWithFFmpegFallback(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer);

    // Media decoder module
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    
    // FFmpeg objects (kept for compatibility and advanced operations)
    AVFormatContext* formatCtx_;  // Access via mediaReader_.getFormatContext()
    AVCodecContext* codecCtx_;   // Access via videoDecoder_.getCodecContext()
    AVFrame* frame_;
    int videoStream_;

    // HAP is intra-frame only - no index needed, every frame is a keyframe
    int64_t frameCount_;
    int64_t lastDecodedPTS_;
    int64_t lastDecodedFrameNo_;
    bool scanComplete_;

    // Video properties
    FrameInfo frameInfo_;
    std::string currentFile_;
    int64_t currentFrame_;
    HAPVariant hapVariant_;

    // Internal state
    bool ready_;
    AVRational frameRateQ_;
    
#ifdef ENABLE_HAP_DIRECT
    // HAP direct decoding
    HapDecoder hapDecoder_;
    bool fallbackWarningShown_;
#endif
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HAPVIDEOINPUT_H

