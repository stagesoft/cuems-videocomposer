#ifndef VIDEOCOMPOSER_NDIVIDEOINPUT_H
#define VIDEOCOMPOSER_NDIVIDEOINPUT_H

#include "LiveInputSource.h"
#include "../video/FrameFormat.h"
#include <string>
#include <vector>
#include <cstdint>

#ifdef HAVE_NDI_SDK
#include <Processing.NDI.Lib.h>
#endif

namespace videocomposer {

/**
 * NDIVideoInput - NDI video input source using NDI SDK directly
 * 
 * Receives video streams over NDI protocol. Uses async frame capture
 * via LiveInputSource base class to prevent frame drops.
 */
class NDIVideoInput : public LiveInputSource {
public:
    NDIVideoInput();
    virtual ~NDIVideoInput();

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override;

    // NDI-specific
    static std::vector<std::string> discoverSources(int timeoutMs = 5000);
    void setTallyState(bool onProgram, bool onPreview);
    
    // Connection settings
    void setConnectionTimeout(int timeoutMs) { connectionTimeoutMs_ = timeoutMs; }
    void setDiscoveryTimeout(int timeoutMs) { discoveryTimeoutMs_ = timeoutMs; }
    
    // Get source name (for logging/display)
    const std::string& getSourceName() const { return sourceName_; }

protected:
    // LiveInputSource interface
    bool captureFrame(FrameBuffer& buffer) override;
    const char* getSourceTypeName() const override { return "NDI"; }

private:
    bool initializeNDI();
    void shutdownNDI();
    bool connectToSource(const std::string& sourceName);

#ifdef HAVE_NDI_SDK
    NDIlib_recv_instance_t ndiReceiver_;
    NDIlib_find_instance_t ndiFinder_;
#endif

    FrameInfo frameInfo_;
    std::string sourceName_;
    bool ready_;
    std::atomic<int64_t> frameCount_;
    
    // Timeouts (milliseconds)
    int connectionTimeoutMs_ = 5000;  // Time to wait for first frame
    int discoveryTimeoutMs_ = 2000;   // Time to wait for source discovery
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_NDIVIDEOINPUT_H

