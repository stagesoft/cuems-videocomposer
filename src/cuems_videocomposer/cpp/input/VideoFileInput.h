#ifndef VIDEOCOMPOSER_VIDEOFILEINPUT_H
#define VIDEOCOMPOSER_VIDEOFILEINPUT_H

#include "InputSource.h"
#include "HardwareDecoder.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <cuems_mediadecoder/VideoDecoder.h>
#include <string>
#include <memory>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace videocomposer {

// Forward declarations
#ifdef HAVE_VAAPI_INTEROP
class VaapiInterop;
class DisplayBackend;
#endif

/**
 * VideoFileInput - FFmpeg-based video file input source
 * 
 * Implements InputSource interface for reading video files using FFmpeg/libav.
 * This is the only input source implementation for now, but the architecture
 * is ready for future implementations (live video, streaming, etc.).
 */
class VideoFileInput : public InputSource {
public:
    VideoFileInput();
    virtual ~VideoFileInput();

    enum class HardwareDecodePreference {
        AUTO,
        SOFTWARE_ONLY,
        VAAPI,
        CUDA
    };

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override;
    bool seek(int64_t frameNumber) override;
    void resetSeekState() override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override;
    DecodeBackend getOptimalBackend() const override;

    // Hardware decoding support
    /**
     * Read a frame directly to GPU texture (for hardware-decoded frames)
     * @param frameNumber Frame number to read
     * @param textureBuffer GPUTextureFrameBuffer to store the decoded texture
     * @return true on success, false on failure
     */
    bool readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer);

    // Additional methods specific to video files
    void setIgnoreStartOffset(bool ignore) { ignoreStartOffset_ = ignore; }
    bool getIgnoreStartOffset() const { return ignoreStartOffset_; }

    void setNoIndex(bool noIndex) { noIndex_ = noIndex; }
    bool getNoIndex() const { return noIndex_; }

    void setHardwareDecodePreference(HardwareDecodePreference preference) { hwPreference_ = preference; }

#ifdef HAVE_VAAPI_INTEROP
    /**
     * Set DisplayBackend for creating per-instance VAAPI interop
     * @param displayBackend DisplayBackend instance with VAAPI support
     */
    void setDisplayBackend(DisplayBackend* displayBackend);
    
    /**
     * Check if zero-copy VAAPI decoding is available
     */
    bool hasVaapiZeroCopy() const;
#endif

private:
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

    bool initializeFFmpeg();
    bool openCodec();
    bool openHardwareCodec();
    bool indexFrames();
    bool isIntraFrameCodec() const;  // Check if codec is intra-frame only (all keyframes)
    void setupDirectSeekMode();      // Setup direct seek mode for intra-frame codecs
    bool seekToFrame(int64_t frameNumber);
    bool seekByTimestamp(int64_t frameNumber);
    int64_t parsePTSFromFrame(AVFrame* frame);
    bool transferHardwareFrameToGPU(AVFrame* hwFrame, GPUTextureFrameBuffer& textureBuffer);
    void cleanup();

    // Media decoder module
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    
    // FFmpeg objects (kept for compatibility and advanced operations)
    AVFormatContext* formatCtx_;  // Access via mediaReader_.getFormatContext()
    AVCodecContext* codecCtx_;   // Access via videoDecoder_.getCodecContext()
    AVFrame* frame_;
    AVFrame* frameFMT_;
    SwsContext* swsCtx_;
    int swsCtxWidth_;
    int swsCtxHeight_;
    AVPixelFormat swsCtxFormat_;   // Track source format for sws context recreation
    int videoStream_;

    // Hardware decoding
    AVBufferRef* hwDeviceCtx_;        // Hardware device context
    AVFrame* hwFrame_;                // Hardware frame (for hardware decoding)
    HardwareDecoder::Type hwDecoderType_;  // Type of hardware decoder in use
    bool useHardwareDecoding_;        // Whether hardware decoding is enabled
    bool codecCtxAllocated_;          // Whether codecCtx_ was allocated separately (hardware) or is part of stream (software)
    HardwareDecodePreference hwPreference_;
    
#ifdef HAVE_VAAPI_INTEROP
    std::unique_ptr<VaapiInterop> vaapiInterop_;  // VAAPI zero-copy interop (owned per-instance)
    DisplayBackend* displayBackend_;  // DisplayBackend for initializing interop (not owned)
#endif

    // Frame indexing
    FrameIndex* frameIndex_;
    int64_t frameCount_;
    int64_t lastDecodedPTS_;
    int64_t lastDecodedFrameNo_;
    bool scanComplete_;
    bool byteSeek_;
    bool noIndex_;

    // Video properties
    FrameInfo frameInfo_;
    std::string currentFile_;
    bool ignoreStartOffset_;
    int64_t currentFrame_;

    // Internal state
    bool ready_;
    AVRational frameRateQ_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOFILEINPUT_H

