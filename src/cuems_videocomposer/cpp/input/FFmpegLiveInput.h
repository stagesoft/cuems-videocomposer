#ifndef VIDEOCOMPOSER_FFMPEGLIVEINPUT_H
#define VIDEOCOMPOSER_FFMPEGLIVEINPUT_H

#include "LiveInputSource.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <cuems_mediadecoder/VideoDecoder.h>
#include <string>

namespace videocomposer {

/**
 * FFmpegLiveInput - Live input via FFmpeg (V4L2, RTSP, etc.)
 * 
 * Stopgap implementation using FFmpeg for live sources.
 * Later can be replaced with dedicated V4L2Input if needed.
 */
class FFmpegLiveInput : public LiveInputSource {
public:
    FFmpegLiveInput();
    virtual ~FFmpegLiveInput();

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override;

    // Set FFmpeg format (e.g., "v4l2", "rtsp")
    void setFormat(const std::string& format) { format_ = format; }

protected:
    // LiveInputSource interface
    bool captureFrame(FrameBuffer& buffer) override;
    const char* getSourceTypeName() const override { return format_.empty() ? "FFmpegLive" : format_.c_str(); }

private:
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    FrameInfo frameInfo_;
    std::string format_;
    bool ready_;
    std::atomic<int64_t> frameCount_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FFMPEGLIVEINPUT_H

