#ifndef VIDEOCOMPOSER_INPUTSOURCE_H
#define VIDEOCOMPOSER_INPUTSOURCE_H

#include "../video/FrameBuffer.h"
#include "../video/FrameFormat.h"
#include <string>
#include <cstdint>

namespace videocomposer {

/**
 * Abstract base class for all input sources.
 * 
 * This interface allows different input types (video files, live video, streaming, etc.)
 * to be used interchangeably. Each layer can have its own input source.
 */
class InputSource {
public:
    virtual ~InputSource() = default;

    /**
     * Open the input source
     * @param source Path or identifier for the source
     * @return true on success, false on failure
     */
    virtual bool open(const std::string& source) = 0;

    /**
     * Close the input source and release resources
     */
    virtual void close() = 0;

    /**
     * Check if input source is ready/opened
     * @return true if ready, false otherwise
     */
    virtual bool isReady() const = 0;

    /**
     * Read a frame at the given frame number
     * @param frameNumber Frame number to read
     * @param buffer FrameBuffer to store the decoded frame
     * @return true on success, false on failure
     */
    virtual bool readFrame(int64_t frameNumber, FrameBuffer& buffer) = 0;

    /**
     * Seek to a specific frame number
     * @param frameNumber Target frame number
     * @return true on success, false on failure
     */
    virtual bool seek(int64_t frameNumber) = 0;
    
    /**
     * Reset internal seek optimization state
     * Call this before seek() to force a full seek even if the frame number
     * is the same as the current position. Used for MTC full frame SYSEX
     * position commands where we must seek regardless of current position.
     */
    virtual void resetSeekState() {}

    /**
     * Get information about the video source
     * @return FrameInfo structure with video properties
     */
    virtual FrameInfo getFrameInfo() const = 0;

    /**
     * Get the current frame number (last frame read)
     * @return Current frame number, or -1 if not available
     */
    virtual int64_t getCurrentFrame() const = 0;

    /**
     * Codec types supported by the system
     */
    enum class CodecType {
        HAP,           // HAP codec (standard)
        HAP_Q,         // HAP Q variant (higher quality)
        HAP_ALPHA,     // HAP Alpha variant (with alpha channel)
        H264,          // H.264/AVC
        HEVC,          // H.265/HEVC
        AV1,           // AV1
        SOFTWARE       // Software codec (fallback)
    };

    /**
     * Decode backend types
     */
    enum class DecodeBackend {
        HAP_DIRECT,    // HAP direct GPU texture (zero-copy)
        GPU_HARDWARE,  // GPU hardware decoder (NVDEC, VAAPI, etc.)
        CPU_SOFTWARE   // CPU software decoder
    };

    /**
     * Detect the codec type of this input source
     * @return CodecType enum value
     */
    virtual CodecType detectCodec() const = 0;

    /**
     * Check if this input source supports direct GPU texture decoding
     * HAP codecs can decode directly to OpenGL textures (zero-copy)
     * @return true if direct GPU texture is supported
     */
    virtual bool supportsDirectGPUTexture() const = 0;

    /**
     * Get the optimal decode backend for this input source
     * @return DecodeBackend enum value indicating best decoding method
     */
    virtual DecodeBackend getOptimalBackend() const = 0;

    /**
     * Check if this is a live stream (no seeking, continuous reading)
     * @return true for live streams (NDI, V4L2, RTSP), false for files
     */
    virtual bool isLiveStream() const { return false; }  // Default: not live

    /**
     * For live streams: get the latest available frame
     * Default implementation calls readFrame(0, buffer)
     * @param buffer FrameBuffer to store the decoded frame
     * @return true on success, false on failure
     */
    virtual bool readLatestFrame(FrameBuffer& buffer) {
        return readFrame(0, buffer);
    }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_INPUTSOURCE_H

