#ifndef VIDEOCOMPOSER_HAPVIDEOINPUT_H
#define VIDEOCOMPOSER_HAPVIDEOINPUT_H

#include "InputSource.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <string>
#include <memory>
#include <cstdint>

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

    /**
     * Get OpenGL texture format for this HAP file
     * @return GL_COMPRESSED_RGB_S3TC_DXT1_EXT or GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
     */
    unsigned int getTextureFormat() const;

private:
    enum class HAPVariant {
        HAP,        // Standard HAP (DXT1)
        HAP_Q,      // HAP Q (DXT5, higher quality)
        HAP_ALPHA   // HAP Alpha (DXT5, with alpha channel)
    };

    bool initializeFFmpeg();
    bool findVideoStream();
    bool openCodec();
    bool indexFrames();
    bool seekToFrame(int64_t frameNumber);
    HAPVariant detectHAPVariant();
    void refineHAPVariantFromFrame(AVFrame* frame);
    int64_t parsePTSFromFrame(AVFrame* frame);
    void cleanup();

    // FFmpeg objects
    AVFormatContext* formatCtx_;
    AVCodecContext* codecCtx_;
    AVFrame* frame_;
    int videoStream_;

    // Frame indexing
    struct FrameIndex {
        int64_t pkt_pts;
        int64_t pkt_pos;
        int64_t frame_pts;
        int64_t frame_pos;
        int64_t timestamp;
        int64_t seekpts;
        int64_t seekpos;
        uint8_t key;
    };
    FrameIndex* frameIndex_;
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
    unsigned int textureFormat_; // OpenGL texture format (DXT1 or DXT5)
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HAPVIDEOINPUT_H

