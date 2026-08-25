/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "VideoFileInput.h"
#include "../../ffcompat.h"
#include "../utils/CLegacyBridge.h"
#include "../utils/Logger.h"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

#ifdef HAVE_VAAPI_INTEROP
#include "../hwdec/VaapiInterop.h"
#include "../display/DisplayBackend.h"
#endif

// OpenGL includes
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/version.h>
#include <libavutil/frame.h>
}

namespace videocomposer {

VideoFileInput::VideoFileInput()
    : formatCtx_(nullptr)
    , codecCtx_(nullptr)
    , frame_(nullptr)
    , frameFMT_(nullptr)
    , swsCtx_(nullptr)
    , swsCtxWidth_(0)
    , swsCtxHeight_(0)
    , swsCtxFormat_(AV_PIX_FMT_NONE)
    , videoStream_(-1)
    , hwDeviceCtx_(nullptr)
    , hwDecoderType_(HardwareDecoder::Type::NONE)
    , useHardwareDecoding_(false)
    , hwPreference_(HardwareDecodePreference::AUTO)
#ifdef HAVE_VAAPI_INTEROP
    , vaapiInterop_(nullptr)
    , displayBackend_(nullptr)
#endif
    , frameIndex_(nullptr)
    , frameCount_(0)
    , lastDecodedPTS_(-1)
    , lastDecodedFrameNo_(-1)
    , scanComplete_(false)
    , byteSeek_(false)
    , noIndex_(false)
    , frameInfo_()
    , currentFile_()
    , ignoreStartOffset_(false)
    , currentFrame_(-1)
    , ready_(false)
    , frameRateQ_({1, 1})
    , lastDecodedHWFrame_(-1)
    , useAsyncDecode_(false)
{
}

VideoFileInput::~VideoFileInput() {
    close();
}

bool VideoFileInput::initializeFFmpeg() {
    // FFmpeg should already be initialized globally
    // This is just a placeholder for any per-instance initialization
    return true;
}

AVCodecParameters* VideoFileInput::streamCodecParams() const {
    if (!formatCtx_ || videoStream_ < 0 ||
        videoStream_ >= static_cast<int>(formatCtx_->nb_streams)) {
        return nullptr;
    }
    return formatCtx_->streams[videoStream_]->codecpar;
}

bool VideoFileInput::open(const std::string& source) {
    if (source.empty()) {
        return false;
    }

    // Close any existing file
    close();

    currentFile_ = source;
    ready_ = false;

    // Initialize FFmpeg
    if (!initializeFFmpeg()) {
        return false;
    }

    // Open video file using MediaFileReader
    //
    // Every failure below used to return a bare false, so a failed load produced
    // one message ("Failed to open <path>") for six unrelated causes. Each site
    // now says which step refused and why - see ClickUp 869efh2ma, where a large
    // .mov failed to load and the log could not distinguish a truncated file from
    // an unsupported container or a codec problem.
    if (!mediaReader_.open(source)) {
        LOG_ERROR << "VideoFileInput: cannot open " << source << ": "
                  << mediaReader_.getLastError();
        return false;
    }

    // Get format context for compatibility
    formatCtx_ = mediaReader_.getFormatContext();
    if (!formatCtx_) {
        LOG_ERROR << "VideoFileInput: no format context after opening " << source;
        return false;
    }

    // Find video stream using MediaFileReader
    videoStream_ = mediaReader_.findStream(AVMEDIA_TYPE_VIDEO);
    if (videoStream_ < 0) {
        LOG_ERROR << "VideoFileInput: no video stream in " << source
                  << " (" << mediaReader_.getStreamCount() << " streams present)";
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }

    // Initialize the hardware device (no codec is opened for it), falling back
    // to a software codec when this file cannot be decoded on hardware at all.
    if (!initializeHardwareDevice() && !openCodec()) {
        AVCodecParameters* cp = mediaReader_.getCodecParameters(videoStream_);
        LOG_ERROR << "VideoFileInput: no usable decoder (hardware and software both "
                  << "failed) for codec "
                  << (cp ? avcodec_get_name(cp->codec_id) : "unknown")
                  << " in " << source;
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }

    // Get video properties
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        LOG_ERROR << "VideoFileInput: video stream " << videoStream_
                  << " unavailable in " << source;
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }
    
    // Frame rate
    double framerate = 0.0;
    if (avStream->r_frame_rate.den > 0 && avStream->r_frame_rate.num > 0) {
        framerate = av_q2d(avStream->r_frame_rate);
        frameRateQ_.den = avStream->r_frame_rate.num;
        frameRateQ_.num = avStream->r_frame_rate.den;
    }

    // Dimensions - always from codec parameters, never from codecCtx_.
    // The hardware path no longer opens a codec context of its own (F2), so the
    // old codecCtx_ branch would read null there. codecpar carries the same
    // width/height in both modes.
    int width = 0;
    int height = 0;
    if (AVCodecParameters* codecParams = streamCodecParams()) {
        width = codecParams->width;
        height = codecParams->height;
    }
    
    // Duration
    double duration = mediaReader_.getDuration();

    // Store frame info
    frameInfo_.width = width;
    frameInfo_.height = height;
    frameInfo_.aspect = (float)width / (float)height;
    frameInfo_.framerate = framerate;
    frameInfo_.duration = duration;
    // Use BGRA32 format for OpenGL rendering (matches original xjadeo)
    frameInfo_.format = PixelFormat::BGRA32;

    // Calculate total frames (needed before indexing decision)
    if (framerate > 0 && duration > 0) {
        frameInfo_.totalFrames = (int64_t)(duration * framerate);
    } else {
        // Will be set after indexing if indexing is enabled
        frameInfo_.totalFrames = 0;
    }

    // Index frames (unless --noindex flag is set)
    if (!noIndex_) {
        if (!loadCachedIndex()) {
            // Cache miss or stale – run the full 3-pass indexing
            if (!indexFrames()) {
                LOG_ERROR << "VideoFileInput: frame indexing failed for " << source
                          << " (estimated " << frameInfo_.totalFrames << " frames, "
                          << frameInfo_.duration << "s @ " << frameInfo_.framerate << "fps)";
                cleanup();
                return false;
            }
            // Persist the index so next open() is instant
            saveCachedIndex();
        }

        // If indexing completed, use indexed frame count if available
        if (frameCount_ > 0 && frameInfo_.totalFrames == 0) {
            frameInfo_.totalFrames = frameCount_;
        }
    } else {
        // Without indexing, we rely on duration-based frame calculation
        // and timestamp-based seeking
        scanComplete_ = true; // Mark as "complete" so seeking can work
        frameCount_ = frameInfo_.totalFrames; // Use calculated frame count
    }

    ready_ = true;
    currentFrame_ = -1;
    
    // ------------------------------------------------------------------
    // Decode-path failure ladder
    //
    // The async queue is now the ONLY decoder on the hardware path - there is
    // no synchronous decoder behind it to quietly pick up the slack. So every
    // way it can fail to open gets an explicit outcome here, and each outcome
    // is recorded in LayerHealth.
    //
    // Ordering note: this runs after ready_ = true and after frame indexing.
    // Every failure exit below therefore resets ready_ = false. A false return
    // from open() leaves this instance partially initialized and the caller
    // MUST discard it - the only production caller (AsyncVideoLoader) does,
    // and its destructor runs close()/cleanup(). There is no support for
    // retrying open() on the same instance.
    // ------------------------------------------------------------------
    if (useHardwareDecoding_ && hwDeviceCtx_ && !indexOnly_) {
        std::string tier3Reason;   // non-empty -> drop to software decode
        std::string tier4Reason;   // non-empty -> the load has failed outright

        asyncDecodeQueue_ = std::make_unique<AsyncDecodeQueue>();
        if (asyncDecodeQueue_->open(currentFile_, hwDeviceCtx_,
                                    AsyncDecodeQueue::EXTRA_HW_FRAMES_FULL,
                                    AsyncDecodeQueue::MAX_QUEUE_SIZE)) {
            if (asyncDecodeQueue_->isHardwareDecoding()) {
                // Tier 1: hardware queue on a full pool.
                useAsyncDecode_ = true;
                setHealth(Health::ok, std::string());
                LOG_INFO << "Async decode queue enabled for smooth hardware decoding";
            } else {
                // The queue opened, but on its OWN internal software decoder:
                // its codec whitelist is narrower than FFmpeg's generic
                // hardware detection, so a HW-capable codec outside that list
                // returns true here having quietly gone soft. Those frames are
                // never consumed on this path, so treat it as a hardware
                // refusal and take the real software route instead.
                tier3Reason = "decode queue opened without hardware acceleration";
            }
        } else {
            const AsyncDecodeQueue::OpenFailure why = asyncDecodeQueue_->lastOpenFailure();
            const int averr = asyncDecodeQueue_->lastOpenAVError();
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            if (averr != 0) {
                av_strerror(averr, errbuf, AV_ERROR_MAX_STRING_SIZE);
            }

            switch (why) {
                case AsyncDecodeQueue::OpenFailure::CODEC_OPEN:
                case AsyncDecodeQueue::OpenFailure::DECODER_LOOKUP:
                    // The container is fine; this decoder would not open on
                    // this device. Software can still play it.
                    tier3Reason = std::string("hardware decoder unavailable for this file")
                                + (averr != 0 ? std::string(": ") + errbuf : std::string());
                    break;
                case AsyncDecodeQueue::OpenFailure::DEMUX:
                case AsyncDecodeQueue::OpenFailure::NO_STREAM:
                case AsyncDecodeQueue::OpenFailure::INTERNAL:
                case AsyncDecodeQueue::OpenFailure::NONE:
                default:
                    // Nothing decodable here, or we ran out of memory doing it.
                    // Software decode would fail the same way.
                    tier4Reason = std::string("decode queue could not open the file")
                                + (averr != 0 ? std::string(": ") + errbuf : std::string());
                    break;
            }
        }

        if (!tier3Reason.empty() || !tier4Reason.empty()) {
            asyncDecodeQueue_->close();
            asyncDecodeQueue_.reset();
            useAsyncDecode_ = false;
        }

        if (!tier3Reason.empty()) {
            // Tier 3: software decode - today's behavior, preserved.
            // The device MUST go first: otherwise the software tier runs with a
            // live VAAPI device attached and openCodec() re-allocates frames on
            // top of the ones already allocated for it.
            teardownHardwareDevice();

            if (!openCodec()) {
                // Tier-3 failure is a tier-4 outcome. Without this branch the
                // layer would report a successful load and then never produce a
                // frame - precisely the silent class LayerHealth exists to close.
                const std::string reason = tier3Reason + "; software decode also failed";
                LOG_ERROR << "VideoFileInput: no usable decoder for " << source
                          << " - " << reason;
                setHealth(Health::load_failed, reason);
                ready_ = false;
                return false;
            }

            LOG_WARNING << "VideoFileInput: " << tier3Reason
                        << " - falling back to software decoding for " << source;
            setHealth(Health::sw_fallback, tier3Reason);
        } else if (!tier4Reason.empty()) {
            // Tier 4: nothing to fall back to.
            // Journal-only: the engine's OSC load is fire-and-forget, so the
            // node still reports this cue armed. Closing that gap is F9.
            LOG_ERROR << "VideoFileInput: " << tier4Reason << " (" << source << ")";
            setHealth(Health::load_failed, tier4Reason);
            ready_ = false;
            return false;
        }
    }

    return true;
}

void VideoFileInput::close() {
    // Stop async decode queue first
    if (asyncDecodeQueue_) {
        asyncDecodeQueue_->close();
        asyncDecodeQueue_.reset();
    }
    useAsyncDecode_ = false;

    cleanup();
    currentFile_.clear();
    ready_ = false;
    currentFrame_ = -1;
    frameInfo_ = {};
    
    // NOTE: Don't reset C globals here. In multi-layer scenarios,
    // resetting would break other layers that are still using the globals.
    // The globals will be reset when the application exits or when
    // the last layer is removed.
}

bool VideoFileInput::isReady() const {
    return ready_ && mediaReader_.isReady();
}

// Helper function to check if codec parameters are compatible with hardware decoding
static bool isCompatibleWithHardwareDecoder(AVCodecParameters* codecParams, HardwareDecoder::Type hwType) {
    if (!codecParams) {
        return false;
    }
    
    AVCodecID codecId = codecParams->codec_id;
    
    // Check for known incompatibilities based on bit depth and profile
    if (codecId == AV_CODEC_ID_H264) {
        // H.264 hardware decoders typically only support 8-bit (baseline, main, high profiles)
        // High 10 Profile (profile 110) is 10-bit and not supported by most hardware decoders
        // Check bits_per_raw_sample first (most reliable)
        // Note: 0 means unknown/unset and typically defaults to 8-bit, so only check if > 8
        if (codecParams->bits_per_raw_sample > 8) {
            LOG_INFO << "H.264 video has " << codecParams->bits_per_raw_sample 
                     << "-bit depth, not supported by hardware decoder, falling back to software";
            return false;
        }
        
        // Also check profile: High 10 Profile (110) is 10-bit
        // Baseline = 66, Main = 77, High = 100, High 10 = 110
        // Check if profile value is 110 (High 10 Profile)
        if (codecParams->profile == 110) {
            LOG_INFO << "H.264 High 10 Profile detected (10-bit), not supported by hardware decoder, falling back to software";
            return false;
        }
    } else if (codecId == AV_CODEC_ID_HEVC) {
        // HEVC hardware decoder support varies, but many don't support 12-bit
        // Check bits_per_raw_sample (0 means unknown/unset)
        if (codecParams->bits_per_raw_sample > 10) {
            LOG_INFO << "HEVC video has " << codecParams->bits_per_raw_sample 
                     << "-bit depth, may not be supported by hardware decoder, falling back to software";
            return false;
        }
    }
    
    // Additional compatibility check: try to query hardware decoder capabilities
    // Get the codec we would use for hardware decoding
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* codec = nullptr;
#else
    AVCodec* codec = nullptr;
#endif
    
    // Find the appropriate hardware codec
    switch (hwType) {
        case HardwareDecoder::Type::VAAPI:
        case HardwareDecoder::Type::VIDEOTOOLBOX:
            // These use standard decoder with hw_device_ctx
            codec = avcodec_find_decoder(codecId);
            break;
        case HardwareDecoder::Type::CUDA: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_cuvid").c_str());
            break;
        }
        case HardwareDecoder::Type::QSV: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_qsv").c_str());
            break;
        }
        case HardwareDecoder::Type::DXVA2: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_dxva2").c_str());
            break;
        }
        default:
            return false;
    }
    
    if (!codec) {
        return false;
    }
    
    // Check hardware configurations to see if the decoder supports this format
    AVHWDeviceType hwDeviceType = HardwareDecoder::getFFmpegDeviceType(hwType);
    if (hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }
    
    // Check if there's a hardware config that supports this device type
    bool hasCompatibleConfig = false;
    for (int n = 0; ; n++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, n);
        if (!cfg) {
            break;
        }
        
        if (cfg->device_type == hwDeviceType) {
            hasCompatibleConfig = true;
            break;
        }
    }
    
    if (!hasCompatibleConfig) {
        LOG_VERBOSE << "No compatible hardware configuration found for codec, will use software";
        return false;
    }
    
    return true;
}

bool VideoFileInput::initializeHardwareDevice() {
    // Create the hardware DEVICE context and confirm this codec can be decoded
    // on it. No codec is opened here: the async decode queue opens the only
    // decoder, against the device this function creates.
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        return false;
    }
    
    AVCodecID codecId = codecParams->codec_id;
    std::string codecName = avcodec_get_name(codecId);

    if (hwPreference_ == HardwareDecodePreference::SOFTWARE_ONLY) {
        LOG_VERBOSE << "Hardware decoding disabled via preference, using software decoder";
        return false;
    }

    LOG_INFO << "Attempting to initialize hardware device for codec: " << codecName;

    bool forceSpecificDecoder = false;
    HardwareDecoder::Type forcedType = HardwareDecoder::Type::NONE;
    switch (hwPreference_) {
        case HardwareDecodePreference::VAAPI:
            forceSpecificDecoder = true;
            forcedType = HardwareDecoder::Type::VAAPI;
            break;
        case HardwareDecodePreference::CUDA:
            forceSpecificDecoder = true;
            forcedType = HardwareDecoder::Type::CUDA;
            break;
        default:
            break;
    }

    if (forceSpecificDecoder) {
        hwDecoderType_ = forcedType;
    } else {
        hwDecoderType_ = HardwareDecoder::detectAvailable();
    }

    if (hwDecoderType_ == HardwareDecoder::Type::NONE) {
        if (forceSpecificDecoder) {
            LOG_WARNING << "Requested hardware decoder (" << HardwareDecoder::getName(forcedType)
                        << ") not available on this system, falling back to software decoding";
        }
        return false;
    }

    if (!HardwareDecoder::isAvailableForCodec(codecId, hwDecoderType_)) {
        if (forceSpecificDecoder) {
            LOG_WARNING << "Requested hardware decoder (" << HardwareDecoder::getName(forcedType)
                        << ") is not available for codec " << codecName << ", falling back to software";
        }
        return false;
    }

    // Check if the codec parameters are compatible with the hardware decoder
    // This detects incompatibilities like 10-bit H.264, unsupported profiles, etc.
    if (!isCompatibleWithHardwareDecoder(codecParams, hwDecoderType_)) {
        LOG_INFO << "Codec format not compatible with hardware decoder, falling back to software";
        return false;
    }

    // Find hardware decoder
    // NOTE: Different hardware decoders work differently:
    // - QSV/CUVID/DXVA2: Have dedicated wrapper decoders (h264_qsv, h264_cuvid)
    // - VAAPI/VideoToolbox: Use standard decoder with hw_device_ctx attached
    
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* hwCodec = nullptr;
#else
    AVCodec* hwCodec = nullptr;
#endif
    std::string hwCodecName;

    switch (hwDecoderType_) {
        case HardwareDecoder::Type::VAAPI:
        case HardwareDecoder::Type::VIDEOTOOLBOX:
            // VAAPI and VideoToolbox use the standard decoder with hw_device_ctx (hwaccel method)
            hwCodec = avcodec_find_decoder(codecId);
            hwCodecName = codecName + " (with " + HardwareDecoder::getName(hwDecoderType_) + " hwaccel)";
            break;
        case HardwareDecoder::Type::CUDA:
            // FFmpeg uses "cuvid" suffix for NVIDIA CUDA decoders
            hwCodecName = codecName + "_cuvid";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        case HardwareDecoder::Type::QSV:
            hwCodecName = codecName + "_qsv";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        case HardwareDecoder::Type::DXVA2:
            hwCodecName = codecName + "_dxva2";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        default:
            return false;
    }

    if (!hwCodec) {
        LOG_WARNING << "Hardware decoder " << hwCodecName << " not found, falling back to software";
        return false;
    }
    
    LOG_INFO << "Found hardware decoder: " << hwCodecName;

    // Get hardware device type first (needed to filter hw_configs)
    AVHWDeviceType hwDeviceType = HardwareDecoder::getFFmpegDeviceType(hwDecoderType_);
    if (hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

    // NOTE the hw-config probe that used to sit here (METHOD_HW_DEVICE_CTX /
    // HW_FRAMES_CTX / INTERNAL, and the pixel format it picked) is gone with the
    // codec open it fed: those values were only ever used to configure the
    // synchronous codec context. The queue's own decoder negotiates the pixel
    // format through FFmpeg's generic get_format path.

    // Create hardware device context (needed for frame transfers even if not for codec)
    int ret = -1;
    
#ifdef HAVE_VAAPI_INTEROP
    // For VAAPI: use shared VADisplay from VaapiInterop for zero-copy support
    // This ensures the decoder and EGL interop use the same VAAPI display
    if (hwDeviceType == AV_HWDEVICE_TYPE_VAAPI && vaapiInterop_ && vaapiInterop_->getVADisplay()) {
        VADisplay sharedDisplay = vaapiInterop_->getVADisplay();
        
        hwDeviceCtx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
        if (hwDeviceCtx_) {
            AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx_->data;
            AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
            vactx->display = sharedDisplay;
            
            ret = av_hwdevice_ctx_init(hwDeviceCtx_);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                LOG_WARNING << "Failed to init shared VAAPI device: " << errbuf;
                av_buffer_unref(&hwDeviceCtx_);
                hwDeviceCtx_ = nullptr;
            } else {
                LOG_INFO << "Using shared VADisplay for VAAPI zero-copy";
            }
        }
    }
#endif
    
    // Fallback: create device context normally (creates its own display)
    if (!hwDeviceCtx_) {
        ret = av_hwdevice_ctx_create(&hwDeviceCtx_, hwDeviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            hwDeviceCtx_ = nullptr;
            return false;
        }
    }

    // NOTE no codec is opened here any more. This function used to allocate a
    // second AVCodecContext on this same file and open a synchronous hardware
    // decoder on it, with a VAAPI surface pool of its own, while the async
    // decode queue opened another. Two pools per layer against a fixed VRAM
    // carve-out is what the eviction-storm hang was measured on.
    //
    // The error paths below therefore free the device context only - there is
    // no codec context here to free.

    // Allocate frames (shared scratch, kept for the software read path)
    frame_ = av_frame_alloc();
    if (!frame_) {
        teardownHardwareDevice();
        return false;
    }

    frameFMT_ = av_frame_alloc();
    if (!frameFMT_) {
        teardownHardwareDevice();
        return false;
    }

    useHardwareDecoding_ = true;
    LOG_INFO << "Hardware device ready for " << codecName
             << " (" << HardwareDecoder::getName(hwDecoderType_)
             << ") - decoding runs on the async queue";
    return true;
}

void VideoFileInput::teardownHardwareDevice() {
    // Undo initializeHardwareDevice() completely before the software tier runs.
    // Leaving the VAAPI device alive would keep a driver context (and its
    // display) open on a layer that is about to decode on the CPU, and
    // openCodec() would allocate frame_/frameFMT_ a second time on top of the
    // ones allocated here.
    if (frameFMT_) {
        av_frame_free(&frameFMT_);
        frameFMT_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }
    useHardwareDecoding_ = false;
    hwDecoderType_ = HardwareDecoder::Type::NONE;
}

bool VideoFileInput::openCodec() {
    // Get codec parameters using MediaFileReader
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        LOG_ERROR << "Failed to get codec parameters for video stream";
        return false;
    }

    std::string codecName = avcodec_get_name(codecParams->codec_id);
    LOG_INFO << "Opening software decoder for codec: " << codecName;

    // Open codec using VideoDecoder
    if (!videoDecoder_.openCodec(codecParams)) {
        LOG_ERROR << "Failed to open software decoder for codec: " << codecName;
        return false;
    }

    // Get codec context from VideoDecoder
    codecCtx_ = videoDecoder_.getCodecContext();
    
    LOG_INFO << "Successfully opened software decoder: " << codecName;

    // Allocate frames
    frame_ = av_frame_alloc();
    if (!frame_) {
        videoDecoder_.close();
        codecCtx_ = nullptr;
        return false;
    }

    frameFMT_ = av_frame_alloc();
    if (!frameFMT_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
        videoDecoder_.close();
        codecCtx_ = nullptr;
        return false;
    }

    useHardwareDecoding_ = false;
    LOG_INFO << "Using SOFTWARE decoding for " << codecName;
    return true;
}

// Forward declaration of FrameIndex structure (matches VideoFileInput::FrameIndex layout)
// Note: This is a duplicate struct definition for helper functions since VideoFileInput::FrameIndex is private
struct LocalFrameIndex {
    int64_t pkt_pts;
    int64_t pkt_pos;
    int64_t frame_pts;
    int64_t frame_pos;
    int64_t timestamp;
    int64_t seekpts;
    int64_t seekpos;
    uint8_t key;
};

// Helper function to add index entry (like xjadeo's add_idx)
static int addIndexEntry(LocalFrameIndex* frameIndex, int64_t fcnt, int64_t frames,
                         int64_t ts, int64_t pos, uint8_t key, AVRational fr_Q, AVRational tb) {
    if (fcnt >= frames) {
        // Overflow - will be handled by caller
        return -1;
    }
    
    frameIndex[fcnt].pkt_pts = ts;
    frameIndex[fcnt].pkt_pos = pos;
    frameIndex[fcnt].timestamp = av_rescale_q(fcnt, fr_Q, tb);
    frameIndex[fcnt].key = key;
    frameIndex[fcnt].frame_pts = -1;
    frameIndex[fcnt].frame_pos = -1;
    frameIndex[fcnt].seekpts = 0;
    frameIndex[fcnt].seekpos = 0;
    
    return 0;
}

// Helper function to find keyframe for a given timestamp (like xjadeo's keyframe_lookup_helper)
static int64_t keyframeLookupHelper(LocalFrameIndex* frameIndex, int64_t fcnt,
                                     int64_t last, int64_t ts) {
    if (last >= fcnt) {
        last = fcnt - 1;
    }
    
    for (int64_t i = last; i >= 0; --i) {
        if (!frameIndex[i].key) continue;
        if (frameIndex[i].pkt_pts == AV_NOPTS_VALUE) {
            continue;
        }
        
        // For hardware decoding, frame_pts may not be set (Pass 2 doesn't verify keyframes)
        // In that case, use timestamp instead
        // Note: frame_pts can be -1 (uninitialized) or AV_NOPTS_VALUE (0x8000000000000000)
        int64_t keyframePts = (frameIndex[i].frame_pts != AV_NOPTS_VALUE && frameIndex[i].frame_pts >= 0) 
                              ? frameIndex[i].frame_pts 
                              : frameIndex[i].timestamp;
        
        if (keyframePts <= ts) {
            return i;
        }
    }
    return -1;
}

bool VideoFileInput::isIntraFrameCodec() const {
    // Check if codec is intra-frame only (all frames are keyframes)
    // These codecs don't need indexing - direct seek mode works perfectly
    AVCodecParameters* codecParams = streamCodecParams();
    if (!codecParams) {
        return false;
    }

    AVCodecID codecId = codecParams->codec_id;

    switch (codecId) {
        // Professional intra-frame codecs
        case AV_CODEC_ID_PRORES:     // Apple ProRes (all variants)
        case AV_CODEC_ID_DNXHD:      // Avid DNxHD/DNxHR
        case AV_CODEC_ID_MJPEG:      // Motion JPEG
        case AV_CODEC_ID_MJPEGB:     // Motion JPEG-B
        case AV_CODEC_ID_RAWVIDEO:   // Uncompressed video
        case AV_CODEC_ID_V210:       // Uncompressed 10-bit 4:2:2
        case AV_CODEC_ID_V410:       // Uncompressed 10-bit 4:4:4
        case AV_CODEC_ID_R210:       // Uncompressed RGB 10-bit
        case AV_CODEC_ID_R10K:       // AJA Kona 10-bit RGB
        case AV_CODEC_ID_AVUI:       // Avid Meridien Uncompressed
        case AV_CODEC_ID_AYUV:       // Uncompressed packed 4:4:4
        case AV_CODEC_ID_TARGA_Y216: // Pinnacle TARGA CineWave YUV16
        case AV_CODEC_ID_JPEG2000:   // JPEG 2000 (intra-frame)
        #ifdef AV_CODEC_ID_CINEFORM
        case AV_CODEC_ID_CINEFORM:   // GoPro CineForm
        #endif
            return true;
        
        // HAP is handled by HAPVideoInput, but include here for completeness
        case AV_CODEC_ID_HAP:
        #ifdef AV_CODEC_ID_HAPQ
        case AV_CODEC_ID_HAPQ:
        #endif
        #ifdef AV_CODEC_ID_HAPALPHA
        case AV_CODEC_ID_HAPALPHA:
        #endif
            return true;
        
        default:
            return false;
    }
}

void VideoFileInput::setupDirectSeekMode() {
    // Setup direct seek mode for intra-frame codecs
    // All frames are keyframes, so we can calculate positions mathematically
    
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        return;
    }
    
    AVRational timeBase = avStream->time_base;
    int64_t frames = frameInfo_.totalFrames;
    if (frames <= 0) {
        double duration = mediaReader_.getDuration();
        if (duration > 0 && frameInfo_.framerate > 0) {
            frames = static_cast<int64_t>(duration * frameInfo_.framerate);
        } else {
            frames = 10000; // Default estimate
        }
    }
    
    // Allocate index for direct seek
    frameIndex_ = static_cast<FrameIndex*>(calloc(frames, sizeof(FrameIndex)));
    if (!frameIndex_) {
        return;
    }
    
    // Get first PTS from stream
    int64_t firstPTS = 0;
    if (avStream->start_time != AV_NOPTS_VALUE) {
        firstPTS = avStream->start_time;
    }
    
    // Fill index with calculated positions (all keyframes)
    for (int64_t i = 0; i < frames; ++i) {
        frameIndex_[i].key = 1;  // All keyframes
        frameIndex_[i].pkt_pts = firstPTS + av_rescale_q(i, frameRateQ_, timeBase);
        frameIndex_[i].frame_pts = frameIndex_[i].pkt_pts;
        frameIndex_[i].pkt_pos = -1;  // Use timestamp seeking
        frameIndex_[i].frame_pos = -1;
        frameIndex_[i].timestamp = av_rescale_q(i, frameRateQ_, timeBase);
        frameIndex_[i].seekpts = frameIndex_[i].pkt_pts;
        frameIndex_[i].seekpos = -1;
    }
    
    frameCount_ = frames;
    scanComplete_ = true;
    byteSeek_ = false;  // Use timestamp seeking
}

bool VideoFileInput::indexFrames() {
    // Check if codec is intra-frame only (all keyframes)
    // These codecs don't need the expensive 3-pass indexing
    if (isIntraFrameCodec()) {
        AVCodecParameters* logParams = streamCodecParams();
        const char* codecName = logParams ? avcodec_get_name(logParams->codec_id) : "unknown";
        LOG_INFO << "Codec " << codecName << " is intra-frame only (all keyframes), skipping indexing";
        setupDirectSeekMode();
        return scanComplete_;
    }
    
    // xjadeo-style 3-pass indexing implementation

    frameCount_ = 0;
    frameIndex_ = nullptr;

    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        return false;
    }
    
    AVRational timeBase = avStream->time_base;
    int64_t frames = frameInfo_.totalFrames;
    if (frames <= 0) {
        // Estimate frames from duration if not available
        double duration = mediaReader_.getDuration();
        if (duration > 0 && frameInfo_.framerate > 0) {
            frames = static_cast<int64_t>(duration * frameInfo_.framerate);
        } else {
            frames = 100000; // Large default, will grow as needed
        }
    }
    
    // Allocate index array (cast to our local FrameIndex type for helper functions)
    frameIndex_ = static_cast<VideoFileInput::FrameIndex*>(calloc(frames, sizeof(VideoFileInput::FrameIndex)));
    if (!frameIndex_) {
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        free(frameIndex_);
        frameIndex_ = nullptr;
        return false;
    }
    
    int use_dts = 0;
    int max_keyframe_interval = 0;
    int keyframe_interval = 0;
    int64_t keyframe_byte_pos = 0;
    int64_t keyframe_byte_distance = 0;
    const int keyframe_interval_limit = 300; // xjadeo default
    
    LOG_INFO << "Indexing video (Pass 1: Scanning packets)...";
    
    /* Pass 1: read all packets
     * -> find keyframes
     * -> check if file is complete
     * -> discover max. keyframe distance
     * -> get PTS/DTS of every *packet*
     */
    while (mediaReader_.readPacket(packet) == 0) {
        if (packet->stream_index != videoStream_) {
            av_packet_unref(packet);
            continue;
        }

        int64_t ts = AV_NOPTS_VALUE;
        
        // Try PTS first, fallback to DTS
        if (!use_dts && packet->pts != AV_NOPTS_VALUE) {
            ts = packet->pts;
        }
        if (ts == AV_NOPTS_VALUE) {
            use_dts = 1;
        }
        if (use_dts && packet->dts != AV_NOPTS_VALUE) {
            ts = packet->dts;
        }

        if (ts == AV_NOPTS_VALUE) {
            LOG_WARNING << "Index error: no PTS, nor DTS at frame " << frameCount_;
            av_packet_unref(packet);
            break;
        }
        
        const uint8_t key = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        // Grow array if needed
        if (frameCount_ >= frames) {
            frames *= 2;
            frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, frames * sizeof(FrameIndex)));
            if (!frameIndex_) {
                av_packet_unref(packet);
                av_packet_free(&packet);
                return false;
            }
        }

        if (addIndexEntry(reinterpret_cast<LocalFrameIndex*>(frameIndex_), frameCount_, frames, ts, packet->pos, key, frameRateQ_, timeBase) < 0) {
            av_packet_unref(packet);
            break;
        }
        
        if (key) {
            int64_t byte_distance = packet->pos - keyframe_byte_pos;
            keyframe_byte_pos = packet->pos;
            if (keyframe_byte_distance < byte_distance) {
                keyframe_byte_distance = byte_distance;
            }
        }
        
        av_packet_unref(packet);
        
        if (++keyframe_interval > max_keyframe_interval) {
            max_keyframe_interval = keyframe_interval;
        }
        
        // Check for problematic files (too many frames between keyframes)
        if (max_keyframe_interval > keyframe_interval_limit &&
            (keyframe_byte_distance > 0 && keyframe_byte_distance > 5242880 /* 5 MB */)) {
            LOG_WARNING << "Keyframe interval too large, stopping indexing";
            break;
        }
        
        // Optimization: if first 500 frames are all keyframes, use direct seek mode
        if ((frameCount_ == 500 || frameCount_ == frames) && max_keyframe_interval == 1) {
            int64_t file_frame_offset = frameInfo_.fileFrameOffset;
            int64_t ppts_offset = frameIndex_[0].pkt_pts;
            // Check if file_frame_offset matches packet PTS (like xjadeo)
            if (file_frame_offset == av_rescale_q(ppts_offset, timeBase, frameRateQ_) || file_frame_offset == 0) {
                LOG_INFO << "First 500 frames are all keyframes. Using direct seek mode.";
                // Fill in all frames as keyframes (only if we know total frames)
                if (frameInfo_.totalFrames > 0) {
                    for (int64_t i = 0; i < frameInfo_.totalFrames && i < frames; ++i) {
                        frameIndex_[i].key = 1;
                        frameIndex_[i].pkt_pts = frameIndex_[i].frame_pts = 
                            ppts_offset + av_rescale_q(i, frameRateQ_, timeBase);
                        frameIndex_[i].frame_pos = -1;
                        frameIndex_[i].timestamp = av_rescale_q(file_frame_offset + i, frameRateQ_, timeBase);
                        frameIndex_[i].seekpts = frameIndex_[i].pkt_pts;
                        frameIndex_[i].seekpos = frameIndex_[i].pkt_pos;
                    }
                    frameCount_ = frameInfo_.totalFrames;
                } else {
                    // Use current frame count
                    frameCount_ = frameCount_; // Already set
                }
                av_packet_free(&packet);
                scanComplete_ = true;
                return true;
            }
        }
        
        if (key) {
            keyframe_interval = 0;
        }

        frameCount_++;
    }
    
    av_packet_free(&packet);
    
    if (frameCount_ == 0) {
        LOG_ERROR << "No frames indexed";
        free(frameIndex_);
        frameIndex_ = nullptr;
        return false;
    }
    
    // Resize array to actual frame count
    if (frameCount_ < frames) {
        frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, frameCount_ * sizeof(FrameIndex)));
        if (!frameIndex_) {
            free(frameIndex_);
            frameIndex_ = nullptr;
            return false;
        }
    }
    
    LOG_INFO << "Pass 1 complete: indexed " << frameCount_ << " packets, max keyframe interval: " << max_keyframe_interval;
    
    /* Pass 2: verify keyframes
     * seek to [all] keyframe, decode one frame after
     * the keyframe and check *frame* PTS
     */
    LOG_INFO << "Indexing video (Pass 2: Verifying keyframes)...";
    
    int64_t keyframecount = 0;
    
    // Need a frame for decoding
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            LOG_ERROR << "Failed to allocate frame for indexing";
            free(frameIndex_);
            frameIndex_ = nullptr;
            return false;
        }
    }
    
    for (int64_t i = 0; i < frameCount_; ++i) {
        if (!frameIndex_[i].key) continue;
        
        // Seek to keyframe
        if (!mediaReader_.seek(frameIndex_[i].pkt_pts, videoStream_, AVSEEK_FLAG_BACKWARD)) {
            LOG_WARNING << "IDX2: Seek failed for keyframe " << i;
            continue;
        }
        
        // Flush codec buffers
        // codec->flush was removed in FFmpeg 4.0+, but avcodec_flush_buffers() is always available
        if (codecCtx_) {
            avcodec_flush_buffers(codecCtx_);
        }
        
        // Decode one frame
        bool got_pic = false;
        int64_t pts = AV_NOPTS_VALUE;
        int bailout = 100;
        
        while (!got_pic && --bailout > 0) {
            AVPacket* decodePacket = av_packet_alloc();
            if (!decodePacket) {
                break;
            }
            
            int err = mediaReader_.readPacket(decodePacket);
            if (err < 0) {
                if (err == AVERROR_EOF) {
                    LOG_WARNING << "IDX2: Read/Seek compensate for premature EOF at keyframe " << i;
                    frameIndex_[i].key = 0;
                }
                av_packet_free(&decodePacket);
                break;
            }
            
            if (decodePacket->stream_index == videoStream_) {
                // Send packet to decoder
                err = videoDecoder_.sendPacket(decodePacket);
                if (err < 0 && err != AVERROR(EAGAIN)) {
                    av_packet_free(&decodePacket);
                    break;
                }
                
                // Receive frame from decoder
                err = videoDecoder_.receiveFrame(frame_);
                if (err == 0) {
                    got_pic = true;
                    pts = parsePTSFromFrame(frame_);
                } else if (err != AVERROR(EAGAIN)) {
                    av_packet_free(&decodePacket);
                    break;
                }
            }
            
            av_packet_free(&decodePacket);
        }
        
        if (!got_pic || pts == AV_NOPTS_VALUE) {
            continue;
        }
        
        frameIndex_[i].frame_pts = pts;
        // Use compatibility wrapper from ffcompat.h
        frameIndex_[i].frame_pos = av_frame_get_pkt_pos(frame_);
        if (pts != AV_NOPTS_VALUE) {
            keyframecount++;
        }
    }
    
    LOG_INFO << "Pass 2 complete: verified " << keyframecount << " keyframes";
    
    /* Pass 3: Create Seek-Table
     * -> assign seek-[key]frame to every frame
     */
    LOG_INFO << "Indexing video (Pass 3: Creating seek table)...";
    
    for (int64_t i = 0; i < frameCount_; ++i) {
        int64_t searchLimit = std::min(frameCount_ - 1, i + 2 + max_keyframe_interval);
        int64_t kfi = keyframeLookupHelper(reinterpret_cast<LocalFrameIndex*>(frameIndex_), frameCount_, searchLimit, frameIndex_[i].timestamp);
        
        if (kfi < 0) {
            frameIndex_[i].seekpts = 0;
            frameIndex_[i].seekpos = 0;
        } else {
            frameIndex_[i].seekpts = frameIndex_[kfi].pkt_pts;
            frameIndex_[i].seekpos = frameIndex_[kfi].frame_pos;
        }
    }
    
    LOG_INFO << "Pass 3 complete: seek table created";
    
    // Update total frames if we discovered more frames than estimated
    if (frameCount_ > frameInfo_.totalFrames) {
        frameInfo_.totalFrames = frameCount_;
    }

    scanComplete_ = true;
    return true;
}

bool VideoFileInput::seek(int64_t frameNumber) {
    if (!isReady() || !scanComplete_) {
        return false;
    }

    int64_t targetFrame = frameNumber;
    if (ignoreStartOffset_) {
        targetFrame += frameInfo_.fileFrameOffset;
    }

    if (targetFrame < 0) {
        return false;
    }

    // If no indexing, use timestamp-based seeking
    if (noIndex_ || !frameIndex_) {
        // Check bounds using totalFrames instead of frameCount_
        if (frameInfo_.totalFrames > 0 && targetFrame >= frameInfo_.totalFrames) {
            return false;
        }
        return seekByTimestamp(targetFrame);
    }

    // With indexing, check bounds and use indexed seeking
    if (targetFrame >= frameCount_) {
        return false;
    }

    return seekToFrame(targetFrame);
}

void VideoFileInput::resetSeekState() {
    // Reset internal tracking to force next seek to actually perform the seek
    // even if seeking to the same frame number (used for MTC full frame SYSEX)
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
}

bool VideoFileInput::seekToFrame(int64_t frameNumber) {
    if (frameNumber < 0 || frameNumber >= frameCount_ || !frameIndex_) {
        return false;
    }

    const FrameIndex& idx = frameIndex_[frameNumber];
    int64_t timestamp = idx.timestamp;

    if (timestamp < 0) {
        return false;
    }

    // Check if we need to seek
    bool needSeek = false;
    if (lastDecodedPTS_ < 0 || lastDecodedFrameNo_ < 0) {
        needSeek = true;
    } else if (lastDecodedPTS_ > timestamp) {
        needSeek = true;
    } else if ((frameNumber - lastDecodedFrameNo_) != 1) {
        if (idx.seekpts != frameIndex_[lastDecodedFrameNo_].seekpts) {
            needSeek = true;
        }
    }

    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;

    if (needSeek) {
        bool seekResult;
        if (byteSeek_ && idx.seekpos > 0) {
            seekResult = mediaReader_.seek(idx.seekpos, videoStream_, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
        } else {
            seekResult = mediaReader_.seek(idx.seekpts, videoStream_, AVSEEK_FLAG_BACKWARD);
        }

        if (!seekResult) {
            return false;
        }

        // Flush codec buffers after seek
        if (codecCtx_) {
            avcodec_flush_buffers(codecCtx_);
        }
    }

    currentFrame_ = frameNumber;
    return true;
}

bool VideoFileInput::seekByTimestamp(int64_t frameNumber) {
    // Seek using timestamp calculation (for files without indexing)
    if (frameInfo_.framerate <= 0.0 || !mediaReader_.isReady() || videoStream_ < 0) {
        return false;
    }

    // Calculate target timestamp from frame number
    double targetTime = (double)frameNumber / frameInfo_.framerate;

    // Seek to the target timestamp using MediaFileReader
    bool seekResult = mediaReader_.seekToTime(targetTime, videoStream_, AVSEEK_FLAG_BACKWARD);
    if (!seekResult) {
        return false;
    }

    // Just flush for all codecs
    if (codecCtx_) {
        avcodec_flush_buffers(codecCtx_);
    }

    currentFrame_ = frameNumber;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    
    return true;
}

bool VideoFileInput::readFrame(int64_t frameNumber, FrameBuffer& buffer) {
    if (!isReady()) {
        return false;
    }

    // This is the SOFTWARE read path: it decodes through videoDecoder_ and reads
    // geometry off codecCtx_. A hardware layer has neither after F2 - codecCtx_ is
    // null and videoDecoder_ was never opened - so the sws property reads below
    // would dereference null. Refuse instead of crashing.
    //
    // Reaching this is a caller bug (LayerPlayback must not fall back to readFrame()
    // for a GPU_HARDWARE backend), so say so once rather than silently per vsync.
    if (useHardwareDecoding_ && !codecCtx_) {
        static bool warnedNoSwContext = false;
        if (!warnedNoSwContext) {
            warnedNoSwContext = true;
            LOG_ERROR << "VideoFileInput::readFrame called on a hardware layer with no "
                      << "software decode context - refusing (frame " << frameNumber << ")";
        }
        return false;
    }

    // QUICK WIN #1: Early return for same frame (xjadeo: if (!force_update && dispFrame == timestamp) return;)
    // If same frame is requested and we have valid decoded data, just re-run color conversion
    // This skips the expensive decode loop but still handles different output buffers
    if (currentFrame_ == frameNumber && frame_ && frame_->data[0]) {
        // Same frame requested - frame_ still has decoded YUV data
        // Just re-run color conversion to output buffer (much faster than re-decoding)
        if (!swsCtx_) {
            // No color conversion context - can't reuse
        } else {
            // Ensure output buffer is allocated
            if (!buffer.isValid() || buffer.info().width != frameInfo_.width ||
                buffer.info().height != frameInfo_.height) {
                FrameInfo outputInfo;
                outputInfo.width = frameInfo_.width;
                outputInfo.height = frameInfo_.height;
                outputInfo.format = PixelFormat::BGRA32;
                buffer.allocate(outputInfo);
            }
            
            // Re-run color conversion (frame_ → buffer)
            uint8_t* dstData[1] = { buffer.data() };
            int dstLinesize[1] = { static_cast<int>(frameInfo_.width * 4) };
            sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, 
                      codecCtx_->height, dstData, dstLinesize);
            return true;
        }
    }

    // Check if we need to seek (optimize for sequential frame access)
    bool needSeek = false;
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        // Check if this is a sequential frame (next frame after current)
        if (currentFrame_ >= 0 && frameNumber == currentFrame_ + 1) {
            // Sequential frame - don't seek, just continue reading
            needSeek = false;
        } else {
            // Non-sequential frame - need to seek
            needSeek = true;
        }
    }
    
    // Seek only if needed
    if (needSeek) {
    if (!seek(frameNumber)) {
            LOG_WARNING << "Failed to seek to frame " << frameNumber;
        return false;
        }
    }

    // Decode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    // Get target timestamp for the frame we want (like xjadeo)
    // Use frame_pts (actual PTS from packet) instead of calculated timestamp
    // This ensures we match against the actual PTS values in the stream
    int64_t targetTimestamp = -1;
    if (frameIndex_ && frameNumber >= 0 && frameNumber < frameCount_) {
        // Use frame_pts if available, fallback to timestamp
        if (frameIndex_[frameNumber].frame_pts >= 0) {
            targetTimestamp = frameIndex_[frameNumber].frame_pts;
        } else {
            targetTimestamp = frameIndex_[frameNumber].timestamp;
        }
    } else if (!noIndex_ && frameInfo_.totalFrames > 0) {
        // Fallback: calculate timestamp from frame number
        AVStream* avStream = mediaReader_.getStream(videoStream_);
        if (!avStream) {
            return false;
        }
        AVRational timeBase = avStream->time_base;
        double fps = av_q2d(av_guess_frame_rate(formatCtx_, avStream, nullptr));
        if (fps > 0) {
            // Calculate timestamp in stream's timebase (like xjadeo)
            AVRational frameRateQ = { static_cast<int>(fps * 1000), 1000 }; // Approximate
            targetTimestamp = av_rescale_q(frameNumber, frameRateQ, timeBase);
        }
    }

    // Decode frames until we get the target frame (like xjadeo's seek_frame)
    // Use PTS matching with fuzzy matching to handle keyframe-based codecs
    int64_t oneFrame = 1;
    if (frameIndex_ && frameNumber > 0 && frameNumber < frameCount_) {
        // Calculate one_frame equivalent (timestamp difference between consecutive frames)
        if (frameIndex_[frameNumber-1].timestamp >= 0 && 
            frameIndex_[frameNumber].timestamp >= 0) {
            oneFrame = frameIndex_[frameNumber].timestamp - frameIndex_[frameNumber-1].timestamp;
            if (oneFrame <= 0) oneFrame = 1;
        }
    }
    const int64_t prefuzz = oneFrame > 10 ? 1 : 0;
    
    // Bailout counts frames we've PASSED the target - prevents infinite loops
    int bailout = 64;
    int maxPackets = 500;
    bool frameFinished = false;
    
    // Track best frame seen (for B-frame reordering)
    int64_t bestPTS = AV_NOPTS_VALUE;
    AVFrame* bestFrame = av_frame_alloc();
    
    while (bailout > 0 && maxPackets > 0) {
        av_packet_unref(packet);
        int err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                // Drain remaining frames
                videoDecoder_.sendPacket(nullptr);
                while ((err = videoDecoder_.receiveFrame(frame_)) == 0) {
                    int64_t pts = parsePTSFromFrame(frame_);
                    if (pts == AV_NOPTS_VALUE) continue;
                    lastDecodedPTS_ = pts;
                    
                    if (targetTimestamp < 0) {
                        frameFinished = true;
                        break;
                    }
                    
                    // Accept frame if it's at or after target
                    if (pts >= targetTimestamp - prefuzz) {
                        if (bestPTS == AV_NOPTS_VALUE || pts < bestPTS) {
                            bestPTS = pts;
                            av_frame_unref(bestFrame);
                            av_frame_ref(bestFrame, frame_);
                        }
                        // If we got exact match or close enough, done
                        if (pts < targetTimestamp + oneFrame) {
                            frameFinished = true;
                            break;
                        }
                    }
                }
                if (frameFinished) break;
                if (bestPTS != AV_NOPTS_VALUE) {
                    av_frame_unref(frame_);
                    av_frame_move_ref(frame_, bestFrame);
                    frameFinished = true;
                    break;
                }
                --bailout;
                continue;
            } else {
                av_frame_free(&bestFrame);
                av_packet_free(&packet);
                return false;
            }
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }
        
        --maxPackets;

        err = videoDecoder_.sendPacket(packet);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            --bailout;
            continue;
        }

        // Drain ALL available frames
        while ((err = videoDecoder_.receiveFrame(frame_)) == 0) {
            int64_t pts = parsePTSFromFrame(frame_);
            if (pts == AV_NOPTS_VALUE) continue;
            
            lastDecodedPTS_ = pts;

            if (targetTimestamp < 0) {
                frameFinished = true;
                break;
            }

            // Track the closest frame to target (for B-frame reordering)
            if (pts >= targetTimestamp - prefuzz && pts < targetTimestamp + oneFrame * 2) {
                // This frame is in acceptable range - track the closest to target
                int64_t ptsDiff = (pts >= targetTimestamp) ? (pts - targetTimestamp) : (targetTimestamp - pts);
                int64_t bestDiff = (bestPTS != AV_NOPTS_VALUE) ? 
                    ((bestPTS >= targetTimestamp) ? (bestPTS - targetTimestamp) : (targetTimestamp - bestPTS)) : INT64_MAX;
                if (bestPTS == AV_NOPTS_VALUE || ptsDiff < bestDiff) {
                    bestPTS = pts;
                    av_frame_unref(bestFrame);
                    av_frame_ref(bestFrame, frame_);
                }
                
                // Exact match - done immediately
                if (pts >= targetTimestamp && pts < targetTimestamp + oneFrame) {
                    frameFinished = true;
                    break;
                }
            }
            
            // Only count as bailout if we've gone past target
            if (pts > targetTimestamp + oneFrame) {
                --bailout;
                // If we have an acceptable match, use it
                if (bestPTS != AV_NOPTS_VALUE) {
                    av_frame_unref(frame_);
                    av_frame_move_ref(frame_, bestFrame);
                    frameFinished = true;
                    break;
                }
            }
        }
        
        if (frameFinished) break;
        
        if (err != AVERROR(EAGAIN) && err < 0) {
            --bailout;
        }
    }
    
    // If we have a best match but didn't finish, use it
    if (!frameFinished && bestPTS != AV_NOPTS_VALUE) {
        av_frame_unref(frame_);
        av_frame_move_ref(frame_, bestFrame);
        frameFinished = true;
    }
    
    av_frame_free(&bestFrame);

    if (!frameFinished) {
        // Log detailed error information for debugging format-specific issues
        if (targetTimestamp >= 0) {
            LOG_WARNING << "Failed to decode target frame " << frameNumber 
                       << " (target PTS: " << targetTimestamp 
                       << ", last decoded PTS: " << (lastDecodedPTS_ >= 0 ? std::to_string(lastDecodedPTS_) : "none")
                       << ", bailout expired)";
        } else {
            LOG_WARNING << "Failed to decode frame " << frameNumber 
                       << " (no target timestamp, bailout expired)";
        }
        return false;
    }

    // Allocate buffer if needed
    if (!buffer.isValid() || buffer.info().width != frameInfo_.width || 
        buffer.info().height != frameInfo_.height) {
        if (!buffer.allocate(frameInfo_)) {
            return false;
        }
    }

    // Convert frame format using sws_scale (YUV to RGB)
    // Initialize sws context if needed
    if (!swsCtx_ || swsCtxWidth_ != frameInfo_.width || swsCtxHeight_ != frameInfo_.height) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        // Convert from source format to BGRA32 for OpenGL
        // Use SWS_BILINEAR for better real-time performance (mpv default for scaling)
        // SWS_BICUBIC is higher quality but significantly slower for 10-bit content
        swsCtx_ = sws_getContext(
            codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = frameInfo_.width;
        swsCtxHeight_ = frameInfo_.height;
    }

    // Calculate BGRA buffer stride (BGRA32 = 4 bytes per pixel)
    int bgraStride = frameInfo_.width * 4;
    
    // Prepare destination frame (BGRA32, packed format - single plane)
    uint8_t* dstData[4] = {buffer.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {bgraStride, 0, 0, 0};
    
    // Scale and convert YUV to RGB
    int result = sws_scale(swsCtx_,
              (const uint8_t* const*)frame_->data, frame_->linesize,
              0, codecCtx_->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
        return false;
    }

    int64_t pts = parsePTSFromFrame(frame_);
    if (pts != AV_NOPTS_VALUE) {
        lastDecodedPTS_ = pts;
        lastDecodedFrameNo_ = frameNumber;
    }

    currentFrame_ = frameNumber;

    return true;
}

int64_t VideoFileInput::parsePTSFromFrame(AVFrame* frame) {
    int64_t pts = AV_NOPTS_VALUE;
    
    // Match xjadeo's parse_pts_from_frame logic
    // Try best effort timestamp first (using compatibility wrapper from ffcompat.h)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(51, 49, 100)
    if (pts == AV_NOPTS_VALUE) {
        pts = av_frame_get_best_effort_timestamp(frame);
    }
#endif
    
    // Fallback: pkt_pts was removed in FFmpeg 4.0 (libavutil 56.x+)
    // Use frame->pts instead (which should work in all versions)
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    
    // Fallback to frame pts (may be bogus with many codecs)
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    
    // Last resort: packet DTS
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pkt_dts;
    }

    return pts;
}

FrameInfo VideoFileInput::getFrameInfo() const {
    return frameInfo_;
}

int64_t VideoFileInput::getCurrentFrame() const {
    return currentFrame_;
}

InputSource::CodecType VideoFileInput::detectCodec() const {
    // Codec identity comes from codecpar, NOT from codecCtx_.
    // Keying off codecCtx_ meant "SOFTWARE whenever no sync context is open",
    // which after F2 is every hardware layer - it would route the whole fleet
    // to CPU_SOFTWARE through getOptimalBackend().
    AVCodecParameters* codecParams = streamCodecParams();
    if (!codecParams) {
        return CodecType::SOFTWARE;
    }

    const AVCodecID codecId = codecParams->codec_id;

    // Check for HAP codec
    // Note: FFmpeg may use AV_CODEC_ID_HAP for all HAP variants
    // The specific variant (HAP, HAP_Q, HAP_ALPHA) is determined by HAPVideoInput
    if (codecId == AV_CODEC_ID_HAP) {
        // Check for variant-specific codec IDs if available
        #ifdef AV_CODEC_ID_HAPALPHA
        if (codecId == AV_CODEC_ID_HAPALPHA) {
            return CodecType::HAP_ALPHA;
        }
        #endif
        #ifdef AV_CODEC_ID_HAPQ
        if (codecId == AV_CODEC_ID_HAPQ) {
            return CodecType::HAP_Q;
        }
        #endif
        // Default to standard HAP - HAPVideoInput will detect the actual variant
        return CodecType::HAP;
    }

    // Check for hardware-accelerated codecs
    if (codecId == AV_CODEC_ID_H264) {
        return CodecType::H264;
    }
    if (codecId == AV_CODEC_ID_HEVC) {
        return CodecType::HEVC;
    }
    if (codecId == AV_CODEC_ID_AV1) {
        return CodecType::AV1;
    }

    // Default to software codec
    return CodecType::SOFTWARE;
}

bool VideoFileInput::supportsDirectGPUTexture() const {
    // Only HAP codecs support direct GPU texture decoding
    return detectCodec() == CodecType::HAP ||
           detectCodec() == CodecType::HAP_Q ||
           detectCodec() == CodecType::HAP_ALPHA;
}

InputSource::DecodeBackend VideoFileInput::getOptimalBackend() const {
    CodecType codec = detectCodec();
    
    // HAP codecs use direct GPU texture (zero-copy)
    if (codec == CodecType::HAP || codec == CodecType::HAP_Q || codec == CodecType::HAP_ALPHA) {
        return DecodeBackend::HAP_DIRECT;
    }
    
    // Hardware-accelerated codecs (H.264, HEVC, AV1) can use GPU hardware decoder
    if (codec == CodecType::H264 || codec == CodecType::HEVC || codec == CodecType::AV1) {
        // useHardwareDecoding_ is authoritative: it is what open() actually
        // settled on for THIS file, after the compatibility gates and the
        // failure ladder had their say.
        //
        // The capability probe that used to stand in when codecCtx_ was null is
        // deliberately gone. It answered "does a hardware decoder exist for this
        // codec id", which is a property of the machine, not of the file - so a
        // 10-bit H.264 rejected by the compatibility gate and correctly running
        // in software would still be reported GPU_HARDWARE, sending the caller
        // down a path this input cannot serve. And with the sync decoder removed
        // codecCtx_ is null for every hardware layer, so that branch would now
        // be the common case rather than the exception.
        return useHardwareDecoding_ ? DecodeBackend::GPU_HARDWARE
                                    : DecodeBackend::CPU_SOFTWARE;
    }
    
    // Software codecs use CPU
    return DecodeBackend::CPU_SOFTWARE;
}

bool VideoFileInput::readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    if (!isReady() || !useHardwareDecoding_) {
        // Hardware decoding not available or not enabled
        return false;
    }

    // Lazy initialization of VaapiInterop (needs GL context which may not be available at open time)
#ifdef HAVE_VAAPI_INTEROP
    if (!vaapiInterop_ && displayBackend_ && displayBackend_->hasVaapiSupport() &&
        hwDecoderType_ == HardwareDecoder::Type::VAAPI) {
        vaapiInterop_ = std::make_unique<VaapiInterop>();
        if (!vaapiInterop_->init(displayBackend_)) {
            LOG_WARNING << "Failed to initialize per-instance VaapiInterop (async path)";
            vaapiInterop_.reset();
        }
    }
#endif

    // =========================================================================
    // DECODE PATH (mpv-style async queue - the only decoder on this path)
    // =========================================================================
    if (useAsyncDecode_ && asyncDecodeQueue_) {
        // Set target frame so decode thread knows where we are
        asyncDecodeQueue_->setTargetFrame(frameNumber);

        // Cold start needs the decode thread to spin up the VAAPI pipeline
        // (~50ms for 4K), but this wait runs ON THE RENDER THREAD and blocks
        // every other layer's output while it holds. The old 200ms was sized to
        // avoid falling through to the synchronous decoder; there is nothing to
        // fall through to now, so wait a fraction of a frame and simply retry on
        // the next vsync - the queue keeps cold-starting in the background
        // either way.
        const int waitMs = textureBuffer.isValid() ? 5 : 20;
        AVFrame* queuedFrame = asyncDecodeQueue_->getFrame(frameNumber, waitMs);

        if (queuedFrame) {
            // Got frame from queue - transfer to GPU texture
            // Note: vaSyncSurface happens here, but the actual decode already completed in background
            if (transferHardwareFrameToGPU(queuedFrame, textureBuffer, true)) {  // skipSync: already synced by async queue
                return true;
            }
            LOG_WARNING << "Async decode: GPU transfer failed for frame " << frameNumber;
            // A failed transfer must not blank a layer that is already showing
            // something: hold the last good texture and try again next vsync.
            if (textureBuffer.isValid()) {
                return true;
            }
        } else {
            // Frame not ready. Throttled per layer - this counter used to be a
            // function-local static, so one busy layer silenced the others'
            // misses and attributed its own to whichever layer logged first.
            if (++queueMissCount_ % 30 == 1) {
                LOG_WARNING << "Async decode: frame " << frameNumber << " not in queue (oldest="
                           << asyncDecodeQueue_->getOldestFrame() << ", newest="
                           << asyncDecodeQueue_->getNewestFrame() << ")";
            }
            // Hold the last displayed frame rather than blanking the layer.
            if (textureBuffer.isValid()) {
                return true;
            }
        }
    }

    // No synchronous fallback exists any more. The queue is the only decoder
    // on this path; if it had nothing for us and there is no texture to hold,
    // the honest answer is "no frame this vsync".
    //
    // What used to be here decoded the frame inline on the render thread,
    // through a second hardware decoder competing for the same VAAPI pool -
    // which is what progressively exhausted it. See ClickUp 869en65tm.
    return false;
}

bool VideoFileInput::transferHardwareFrameToGPU(AVFrame* hwFrame, GPUTextureFrameBuffer& textureBuffer, bool skipSync) {
    if (!hwFrame) {
        LOG_WARNING << "transferHardwareFrameToGPU: hwFrame is NULL";
        return false;
    }
    
    // For hardware frames (VAAPI, etc.), data[0] may be NULL
    // VAAPI stores VASurfaceID in data[3], not data[0]
    // Check format to determine if it's a valid hardware frame
    bool isHwFrame = (hwFrame->format == AV_PIX_FMT_VAAPI ||
                      hwFrame->format == AV_PIX_FMT_CUDA ||
                      hwFrame->format == AV_PIX_FMT_QSV ||
                      hwFrame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
                      hwFrame->format == AV_PIX_FMT_DXVA2_VLD);
    
    if (!isHwFrame && !hwFrame->data[0]) {
        LOG_WARNING << "transferHardwareFrameToGPU: frame has no data";
        return false;
    }

#ifdef HAVE_VAAPI_INTEROP
    // VAAPI ZERO-COPY PATH: Use shared VADisplay for direct GPU-to-GPU transfer
    // Two-phase import:
    //   Phase 1: createEGLImages - can be done from any thread
    //   Phase 2: bindTexturesToImages - must be done from GL thread
    if (hwFrame->format == AV_PIX_FMT_VAAPI && vaapiInterop_ && vaapiInterop_->isAvailable()) {
        GLuint texY = 0, texUV = 0;
        int width = 0, height = 0;

        // Phase 1: Create EGL images (works on any thread)
        // Skip vaSyncSurface if frame was already synced by the async decode queue
        if (vaapiInterop_->createEGLImages(hwFrame, width, height, skipSync)) {
            // Phase 2: Try to bind textures (needs GL context)
            if (vaapiInterop_->bindTexturesToImages(texY, texUV)) {
                // Set up the texture buffer with the imported textures
                if (!textureBuffer.setExternalNV12Textures(texY, texUV, frameInfo_)) {
                    LOG_WARNING << "transferHardwareFrameToGPU: Failed to set external NV12 textures";
                    vaapiInterop_->releaseFrame();
                    // Fall through to CPU path
                } else {
                    return true;
                }
            } else {
                // Texture binding failed - release EGL images and fall back to CPU path
                vaapiInterop_->releaseFrame();
            }
        } else {
            LOG_VERBOSE << "transferHardwareFrameToGPU: VAAPI zero-copy failed, falling back to CPU path";
        }
    }
#endif

    // Check if this is actually a hardware frame (has hardware format)
    // Cuvid decoders can output frames in different formats - some may be CPU frames already
    bool isHardwareFrame = (hwFrame->format == AV_PIX_FMT_CUDA || 
                           hwFrame->format == AV_PIX_FMT_VAAPI ||
                           hwFrame->format == AV_PIX_FMT_QSV ||
                           hwFrame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
                           hwFrame->format == AV_PIX_FMT_DXVA2_VLD ||
                           hwFrame->hw_frames_ctx != nullptr);

    // Allocate CPU frame
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            return false;
        }
    }

    // Determine which frame to use for conversion
    AVFrame* sourceFrame = hwFrame;  // Use hwFrame directly if it's already on CPU
    
    if (isHardwareFrame) {
        // Hardware frames need to be transferred from GPU to CPU first
        
        // Allocate CPU frame if needed
        if (!frame_) {
            frame_ = av_frame_alloc();
            if (!frame_) {
                return false;
            }
        }
        
        // Download from hardware frame to CPU frame
        int ret = av_hwframe_transfer_data(frame_, hwFrame, 0);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            LOG_WARNING << "Failed to transfer hardware frame to CPU: " << errbuf;
            return false;
        }
        sourceFrame = frame_;
    }
    // else: h264_cuvid outputs directly to CPU memory (e.g., NV12) - use hwFrame directly

    AVPixelFormat srcFormat = static_cast<AVPixelFormat>(sourceFrame->format);
    
    // OPTIMIZATION: If source is NV12, upload directly without sws_scale conversion
    // The shader will do YUV→RGB conversion on the GPU (much faster than CPU sws_scale)
    if (srcFormat == AV_PIX_FMT_NV12) {
        // Allocate NV12 multi-plane texture if needed
        if (!textureBuffer.isValid() || 
            textureBuffer.getPlaneType() != TexturePlaneType::YUV_NV12 ||
            textureBuffer.info().width != frameInfo_.width || 
            textureBuffer.info().height != frameInfo_.height) {
            if (!textureBuffer.allocateMultiPlane(frameInfo_, TexturePlaneType::YUV_NV12)) {
                LOG_WARNING << "transferHardwareFrameToGPU: Failed to allocate NV12 texture";
                return false;
            }
            LOG_INFO << "transferHardwareFrameToGPU: Allocated NV12 multi-plane texture "
                    << frameInfo_.width << "x" << frameInfo_.height;
        }
        
        // Upload Y and UV planes directly
        // NV12: data[0] = Y plane, data[1] = interleaved UV plane
        if (!textureBuffer.uploadMultiPlaneData(
                sourceFrame->data[0],  // Y plane
                sourceFrame->data[1],  // UV plane (interleaved)
                nullptr,               // No V plane for NV12
                sourceFrame->linesize[0],  // Y stride
                sourceFrame->linesize[1],  // UV stride
                0)) {                      // No V stride
            LOG_WARNING << "transferHardwareFrameToGPU: Failed to upload NV12 data";
            return false;
        }
        
        return true;
    }
    
    // OPTIMIZATION: If source is YUV420P, upload directly without sws_scale conversion
    if (srcFormat == AV_PIX_FMT_YUV420P) {
        // Allocate YUV420P multi-plane texture if needed
        if (!textureBuffer.isValid() || 
            textureBuffer.getPlaneType() != TexturePlaneType::YUV_420P ||
            textureBuffer.info().width != frameInfo_.width || 
            textureBuffer.info().height != frameInfo_.height) {
            if (!textureBuffer.allocateMultiPlane(frameInfo_, TexturePlaneType::YUV_420P)) {
                LOG_WARNING << "transferHardwareFrameToGPU: Failed to allocate YUV420P texture";
                return false;
            }
            LOG_INFO << "transferHardwareFrameToGPU: Allocated YUV420P multi-plane texture "
                    << frameInfo_.width << "x" << frameInfo_.height;
        }
        
        // Upload Y, U, V planes directly
        if (!textureBuffer.uploadMultiPlaneData(
                sourceFrame->data[0],  // Y plane
                sourceFrame->data[1],  // U plane
                sourceFrame->data[2],  // V plane
                sourceFrame->linesize[0],  // Y stride
                sourceFrame->linesize[1],  // U stride
                sourceFrame->linesize[2])) { // V stride
            LOG_WARNING << "transferHardwareFrameToGPU: Failed to upload YUV420P data";
            return false;
        }
        
        return true;
    }

    // FALLBACK: For other formats, use sws_scale to convert to RGBA
    // This is slower but ensures compatibility with all pixel formats

    // Guard: hardware formats and AV_PIX_FMT_NONE have no swscale descriptor → crash.
    // If we reach here with such a format (e.g. av_hwframe_transfer_data produced an unexpected
    // result), return false gracefully instead of asserting inside sws_getContext.
    if (!av_pix_fmt_desc_get(srcFormat)) {
        LOG_WARNING << "transferHardwareFrameToGPU: unsupported/hw srcFormat " 
                   << (int)srcFormat << " for sws_scale, skipping frame";
        return false;
    }

    // Allocate RGBA texture buffer if needed
    if (!textureBuffer.isValid() || 
        textureBuffer.getPlaneType() != TexturePlaneType::SINGLE ||
        textureBuffer.info().width != frameInfo_.width || 
        textureBuffer.info().height != frameInfo_.height) {
        GLenum textureFormat = GL_RGBA;
        if (!textureBuffer.allocate(frameInfo_, textureFormat, false)) {
            return false;
        }
    }

    // Initialize sws context if needed
    if (!swsCtx_ || swsCtxWidth_ != sourceFrame->width || swsCtxHeight_ != sourceFrame->height ||
        swsCtxFormat_ != srcFormat) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        // Use SWS_BILINEAR for better real-time performance
        swsCtx_ = sws_getContext(
            sourceFrame->width, sourceFrame->height, srcFormat,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = sourceFrame->width;
        swsCtxHeight_ = sourceFrame->height;
        swsCtxFormat_ = srcFormat;
        LOG_WARNING << "transferHardwareFrameToGPU: Using sws_scale fallback for format " 
                   << av_get_pix_fmt_name(srcFormat) << " (slower path)";
    }

    // Allocate temporary CPU buffer for RGBA data
    int rgbaStride = frameInfo_.width * 4;
    size_t rgbaSize = rgbaStride * frameInfo_.height;
    std::vector<uint8_t> rgbaBuffer(rgbaSize);

    // Prepare destination frame (RGBA)
    uint8_t* dstData[4] = {rgbaBuffer.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {rgbaStride, 0, 0, 0};
    
    // Scale and convert to RGBA
    int result = sws_scale(swsCtx_,
              (const uint8_t* const*)sourceFrame->data, sourceFrame->linesize,
              0, sourceFrame->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
        LOG_WARNING << "transferHardwareFrameToGPU: sws_scale failed, result=" << result;
        return false;
    }

    // Upload RGBA data to GPU texture
    if (!textureBuffer.uploadUncompressedData(rgbaBuffer.data(), rgbaSize,
                                             frameInfo_.width, frameInfo_.height,
                                             GL_RGBA)) {
        return false;
    }

    return true;
}

#ifdef HAVE_VAAPI_INTEROP
void VideoFileInput::setDisplayBackend(DisplayBackend* displayBackend) {
    displayBackend_ = displayBackend;
    // VaapiInterop will be created lazily when needed (requires GL context)
}

bool VideoFileInput::hasVaapiZeroCopy() const {
    return vaapiInterop_ != nullptr && vaapiInterop_->isAvailable() &&
           hwDecoderType_ == HardwareDecoder::Type::VAAPI;
}
#endif

void VideoFileInput::cleanup() {
    // Free frame index
    if (frameIndex_) {
        free(frameIndex_);
        frameIndex_ = nullptr;
    }
    frameCount_ = 0;

    // Free software scaler
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    swsCtxWidth_ = 0;
    swsCtxHeight_ = 0;
    swsCtxFormat_ = AV_PIX_FMT_NONE;

    // Free frames (must use av_frame_free for frames allocated with av_frame_alloc)
    if (frameFMT_) {
        av_frame_free(&frameFMT_);
        frameFMT_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    // Free hardware device context
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }

    // Close the software decoder. There is no hardware branch any more: the
    // hardware path opens no codec context of its own, so codecCtx_ is either
    // videoDecoder_'s (owned and freed by it) or null.
    videoDecoder_.close();
    codecCtx_ = nullptr;

    // Close media reader (closes format context)
    mediaReader_.close();
        formatCtx_ = nullptr;

#ifdef HAVE_VAAPI_INTEROP
    // Clean up per-instance VaapiInterop
    vaapiInterop_.reset();
#endif

    videoStream_ = -1;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    lastDecodedHWFrame_ = -1;
    scanComplete_ = false;
    currentFrame_ = -1;
    useHardwareDecoding_ = false;
    hwDecoderType_ = HardwareDecoder::Type::NONE;
}

// ============================================================================
// Decode queue control and health
// ============================================================================

void VideoFileInput::setLoopMode(bool loop, int64_t totalFrames) {
    if (asyncDecodeQueue_) {
        asyncDecodeQueue_->setLoopMode(loop, totalFrames);
    }
}

void VideoFileInput::setHealth(Health health, const std::string& reason) {
    health_ = health;
    std::lock_guard<std::mutex> lock(healthReasonMutex_);
    healthReason_ = reason;
}

InputSource::Health VideoFileInput::getHealth() const {
    return health_.load();
}

std::string VideoFileInput::getHealthReason() const {
    std::lock_guard<std::mutex> lock(healthReasonMutex_);
    return healthReason_;
}

// ---------------------------------------------------------------------------
// Index caching  (shared by videocomposer runtime and cuems-videoindexer CLI)
// ---------------------------------------------------------------------------

static const char   IDX_MAGIC[4]   = {'C','X','I','D'};
static const uint32_t IDX_VERSION  = 1;

struct IdxHeader {
    char     magic[4];
    uint32_t version;
    int64_t  video_size;
    int64_t  video_mtime;
    int64_t  frameCount;
    int32_t  frameRateQ_num;
    int32_t  frameRateQ_den;
    int32_t  width;
    int32_t  height;
    float    aspect;
    double   framerate;
    int64_t  totalFrames;
    double   duration;
    int64_t  fileFrameOffset;
    uint8_t  scanComplete;
    uint8_t  byteSeek;
    uint8_t  _pad[6]; // align to 8 bytes
};

std::string VideoFileInput::getIndexPath(const std::string& videoPath) {
    // /path/to/media/video.mp4  ->  /path/to/media/indexes/video.mp4.idx
    size_t sep = videoPath.rfind('/');
    std::string dir  = (sep != std::string::npos) ? videoPath.substr(0, sep) : ".";
    std::string base = (sep != std::string::npos) ? videoPath.substr(sep + 1) : videoPath;
    return dir + "/indexes/" + base + ".idx";
}

bool VideoFileInput::isCacheValid(const std::string& videoPath) {
    std::string idxPath = getIndexPath(videoPath);

    struct stat vidSt{}, idxSt{};
    if (stat(videoPath.c_str(), &vidSt) != 0) return false;
    if (stat(idxPath.c_str(), &idxSt) != 0) return false;

    FILE* f = fopen(idxPath.c_str(), "rb");
    if (!f) return false;

    IdxHeader hdr{};
    bool ok = (fread(&hdr, sizeof(hdr), 1, f) == 1);
    fclose(f);

    if (!ok) return false;
    if (memcmp(hdr.magic, IDX_MAGIC, 4) != 0) return false;
    if (hdr.version != IDX_VERSION) return false;
    if (hdr.video_size  != (int64_t)vidSt.st_size)  return false;
    if (hdr.video_mtime != (int64_t)vidSt.st_mtime) return false;
    return true;
}

bool VideoFileInput::loadCachedIndex() {
    if (currentFile_.empty()) return false;

    std::string idxPath = getIndexPath(currentFile_);

    struct stat vidSt{};
    if (stat(currentFile_.c_str(), &vidSt) != 0) return false;

    FILE* f = fopen(idxPath.c_str(), "rb");
    if (!f) return false;

    IdxHeader hdr{};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }

    if (memcmp(hdr.magic, IDX_MAGIC, 4) != 0 ||
        hdr.version    != IDX_VERSION          ||
        hdr.video_size  != (int64_t)vidSt.st_size  ||
        hdr.video_mtime != (int64_t)vidSt.st_mtime ||
        hdr.frameCount  <= 0)
    {
        fclose(f);
        return false;
    }

    size_t bytes = (size_t)hdr.frameCount * sizeof(FrameIndex);
    FrameIndex* idx = static_cast<FrameIndex*>(malloc(bytes));
    if (!idx) { fclose(f); return false; }

    if (fread(idx, sizeof(FrameIndex), (size_t)hdr.frameCount, f) != (size_t)hdr.frameCount) {
        free(idx);
        fclose(f);
        return false;
    }
    fclose(f);

    if (frameIndex_) free(frameIndex_);
    frameIndex_   = idx;
    frameCount_   = hdr.frameCount;
    frameRateQ_   = { hdr.frameRateQ_num, hdr.frameRateQ_den };
    scanComplete_ = hdr.scanComplete != 0;
    byteSeek_     = hdr.byteSeek != 0;

    // Restore frameInfo fields that indexing sets
    frameInfo_.width           = hdr.width;
    frameInfo_.height          = hdr.height;
    frameInfo_.aspect          = hdr.aspect;
    frameInfo_.framerate       = hdr.framerate;
    frameInfo_.totalFrames     = hdr.totalFrames > 0 ? hdr.totalFrames : hdr.frameCount;
    frameInfo_.duration        = hdr.duration;
    frameInfo_.fileFrameOffset = hdr.fileFrameOffset;

    LOG_INFO << "Loaded cached index for " << currentFile_
             << " (" << frameCount_ << " frames)";
    return true;
}

void VideoFileInput::saveCachedIndex() const {
    if (currentFile_.empty() || !frameIndex_ || frameCount_ <= 0) return;

    std::string idxPath = getIndexPath(currentFile_);

    // Create indexes/ directory if needed
    size_t sep = idxPath.rfind('/');
    if (sep != std::string::npos) {
        std::string dir = idxPath.substr(0, sep);
        // mkdir -p equivalent using POSIX
        std::string cmd = "mkdir -p \"" + dir + "\"";
        (void)system(cmd.c_str());
    }

    struct stat vidSt{};
    if (stat(currentFile_.c_str(), &vidSt) != 0) {
        LOG_WARNING << "saveCachedIndex: cannot stat " << currentFile_;
        return;
    }

    IdxHeader hdr{};
    memcpy(hdr.magic, IDX_MAGIC, 4);
    hdr.version        = IDX_VERSION;
    hdr.video_size     = (int64_t)vidSt.st_size;
    hdr.video_mtime    = (int64_t)vidSt.st_mtime;
    hdr.frameCount     = frameCount_;
    hdr.frameRateQ_num = frameRateQ_.num;
    hdr.frameRateQ_den = frameRateQ_.den;
    hdr.width          = frameInfo_.width;
    hdr.height         = frameInfo_.height;
    hdr.aspect         = frameInfo_.aspect;
    hdr.framerate      = frameInfo_.framerate;
    hdr.totalFrames    = frameInfo_.totalFrames;
    hdr.duration       = frameInfo_.duration;
    hdr.fileFrameOffset= frameInfo_.fileFrameOffset;
    hdr.scanComplete   = scanComplete_ ? 1 : 0;
    hdr.byteSeek       = byteSeek_     ? 1 : 0;

    // Write to a temp file and rename (atomic-ish)
    std::string tmpPath = idxPath + ".tmp";
    FILE* f = fopen(tmpPath.c_str(), "wb");
    if (!f) {
        LOG_WARNING << "saveCachedIndex: cannot create " << tmpPath;
        return;
    }

    bool ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1) &&
              (fwrite(frameIndex_, sizeof(FrameIndex), (size_t)frameCount_, f) == (size_t)frameCount_);
    fclose(f);

    if (!ok) {
        remove(tmpPath.c_str());
        LOG_WARNING << "saveCachedIndex: write error for " << idxPath;
        return;
    }

    if (rename(tmpPath.c_str(), idxPath.c_str()) != 0) {
        remove(tmpPath.c_str());
        LOG_WARNING << "saveCachedIndex: rename failed for " << idxPath;
        return;
    }

    LOG_INFO << "Saved index cache: " << idxPath
             << " (" << frameCount_ << " frames)";
}

} // namespace videocomposer

