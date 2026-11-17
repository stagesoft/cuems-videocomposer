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

// Shared layers on software decode: who owns the frame memory when the decode driver is removed (1amfwm7j9r8).
//
// A cue routed to several outputs is one decode driver plus shared secondaries. On the CPU path the driver's
// frame buffer owns the pixels; the source's cache and every secondary's buffer are views of them (the pointer,
// not a copy). LayerManager::removeLayer promotes a secondary and then destroys the driver. The promoted layer
// goes on decoding into its own buffer, and a decoder allocates only when that buffer is not valid or has another
// size: a view left pointing at the destroyed driver's pixels is written into after they are freed.
//
// The rows check ownership, which needs no memory checker. Under AddressSanitizer a row goes on to the decode even
// when the ownership check failed, so the sanitizer names the defect itself; a plain build stops at the failed check
// instead of corrupting its own heap.

#include "TestFramework.h"
#include "../input/InputSource.h"
#include "../layer/LayerManager.h"
#include "../layer/LayerPlayback.h"
#include "../layer/VideoLayer.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"

#include <cstdint>
#include <cstring>
#include <memory>

using namespace videocomposer;

// 1 when the binary is built with AddressSanitizer: GCC defines __SANITIZE_ADDRESS__, clang answers
// __has_feature(address_sanitizer).
#if defined(__SANITIZE_ADDRESS__)
#define ROW_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ROW_UNDER_ASAN 1
#endif
#endif
#ifndef ROW_UNDER_ASAN
#define ROW_UNDER_ASAN 0
#endif

namespace {

// A software decoder that behaves as VideoFileInput::readFrame does with the caller's buffer: it allocates only
// when the buffer is not valid or has another size, then writes the picture. Every byte of frame N is N.
class DecodingSource : public InputSource {
public:
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool isReady() const override { return true; }
    bool seek(int64_t) override { return true; }
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override {
        const FrameInfo info = getFrameInfo();
        if (!buffer.isValid() || buffer.info().width != info.width || buffer.info().height != info.height) {
            if (!buffer.allocate(info)) {
                return false;
            }
        }
        std::memset(buffer.data(), static_cast<int>(frameNumber & 0xff), buffer.size());
        ++loads;
        return true;
    }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 64;
        info.height = 32;
        info.aspect = 2.0f;
        info.framerate = 25.0;
        info.totalFrames = 250;
        info.format = PixelFormat::BGRA32;
        return info;
    }
    int64_t getCurrentFrame() const override { return 0; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
    int loads = 0;
};

// A rolling MTC at the frame the row sets (25 fps content: the MTC frame is the content frame).
class RowClock : public SyncSource {
public:
    int64_t frame = 0;
    bool connect(const char*) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = 1; return frame; }
    int64_t getCurrentFrame() const override { return frame; }
    const char* getName() const override { return "row-clock"; }
    double getFramerate() const override { return 25.0; }
    long getTimeMs() const override { return static_cast<long>(frame * 40); }
};

// One cue on several outputs, set up as the load / load_shared handlers do it.
struct SharedCue {
    LayerManager manager;
    std::shared_ptr<RowClock> clock = std::make_shared<RowClock>();
    DecodingSource* source = nullptr;   // owned by the layers (shared)

    // The decode driver: an ordinary load, marked as driver by the first load_shared.
    int addDriver() {
        auto layer = std::make_unique<VideoLayer>();
        source = new DecodingSource;
        layer->setInputSource(std::unique_ptr<InputSource>(source));
        layer->playback().setSyncSource(std::shared_ptr<SyncSource>(clock));
        layer->playback().setMtcFollow(true);
        layer->playback().setDecodeDriver(true);
        layer->setUpdatePriority(0);
        return manager.addLayer(std::move(layer));
    }

    // A shared secondary of that driver.
    int addSecondary(int driverId) {
        VideoLayer* driver = manager.getLayer(driverId);
        auto layer = std::make_unique<VideoLayer>();
        layer->playback().setInputSource(driver->playback().getSharedInputSource(), true, false);
        layer->playback().setSyncSource(std::shared_ptr<SyncSource>(clock));
        layer->playback().setMtcFollow(true);
        layer->setUpdatePriority(1);
        return manager.addLayer(std::move(layer));
    }

    // One render tick at MTC frame `frame`: the driver first, as the update priorities order it.
    void tick(int64_t frame) {
        clock->frame = frame;
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < manager.getLayerCount(); ++i) {
                VideoLayer* layer = manager.getLayerByIndex(i);
                if (layer && layer->playback().isDecodeDriver() == (pass == 0)) {
                    layer->playback().update();
                }
            }
        }
    }

    const FrameBuffer* cpu(int layerId) {
        VideoLayer* layer = manager.getLayer(layerId);
        if (!layer) {
            return nullptr;
        }
        const FrameBuffer* cpuBuffer = nullptr;
        const GPUTextureFrameBuffer* gpuBuffer = nullptr;
        layer->playback().getFrameBuffer(cpuBuffer, gpuBuffer);
        return cpuBuffer;
    }
};

// The buffer either holds nothing or owns what it points at: it is not a view of someone else's pixels.
bool ownsOrEmpty(const FrameBuffer* buffer) {
    return buffer != nullptr && (!buffer->isValid() || buffer->ownsBuffer());
}

// Every byte of a valid buffer is `value` (first, middle and last are enough for a memset picture).
bool pictureIs(const FrameBuffer* buffer, uint8_t value) {
    if (!buffer || !buffer->isValid()) {
        return false;
    }
    const uint8_t* p = buffer->data();
    return p[0] == value && p[buffer->size() / 2] == value && p[buffer->size() - 1] == value;
}

}  // namespace

// The set-up itself, so that a failure below is about the promotion and not about the rig: the driver owns the
// pixels and the secondary shows them through a view.
bool test_SharedPromotion_SecondaryIsAViewOfTheDriversPixels() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int secondary = cue.addSecondary(driver);
    cue.tick(5);
    TEST_ASSERT_EQ(cue.source->loads, 1);
    const FrameBuffer* driverBuffer = cue.cpu(driver);
    const FrameBuffer* secondaryBuffer = cue.cpu(secondary);
    TEST_ASSERT_TRUE(driverBuffer && driverBuffer->isValid() && driverBuffer->ownsBuffer());
    TEST_ASSERT_TRUE(secondaryBuffer && secondaryBuffer->isValid() && !secondaryBuffer->ownsBuffer());
    TEST_ASSERT_TRUE(secondaryBuffer->data() == driverBuffer->data());
    TEST_ASSERT_TRUE(pictureIs(secondaryBuffer, 5));
    return true;
}

// The defect. The driver is unloaded, a render tick follows, the promoted layer decodes the next frame: the memory
// it decodes into must be alive, so after the promotion its buffer owns what it points at.
bool test_SharedPromotion_PromotedLayerOwnsTheFrameMemory() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int secondary = cue.addSecondary(driver);
    cue.tick(5);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(driver));
    VideoLayer* promoted = cue.manager.getLayer(secondary);
    TEST_ASSERT_TRUE(promoted != nullptr);
    TEST_ASSERT_TRUE(promoted->playback().isDecodeDriver());

    const bool owned = ownsOrEmpty(cue.cpu(secondary));
#if !ROW_UNDER_ASAN
    TEST_ASSERT_TRUE(owned);
#endif
    // The picture does not change with the promotion: still frame 5 until the next decode.
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(secondary), 5));

    cue.tick(6);
    TEST_ASSERT_EQ(cue.source->loads, 2);
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(secondary), 6));
    TEST_ASSERT_TRUE(owned);
    return true;
}

// Three outputs: the secondary that is not promoted keeps a view too. It must point at live memory before the new
// driver decodes (a held frame is drawn from it) and follow the new driver afterwards.
bool test_SharedPromotion_OtherSecondariesKeepALivePicture() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int first = cue.addSecondary(driver);
    const int second = cue.addSecondary(driver);
    cue.tick(5);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(driver));
    TEST_ASSERT_TRUE(cue.manager.getLayer(first)->playback().isDecodeDriver());
    TEST_ASSERT_FALSE(cue.manager.getLayer(second)->playback().isDecodeDriver());

    const bool owned = ownsOrEmpty(cue.cpu(first));
#if !ROW_UNDER_ASAN
    TEST_ASSERT_TRUE(owned);
#endif
    const FrameBuffer* other = cue.cpu(second);
    TEST_ASSERT_TRUE(other && other->isValid() && !other->ownsBuffer());
    TEST_ASSERT_TRUE(other->data() == cue.cpu(first)->data());   // a view of the new owner's pixels
    TEST_ASSERT_TRUE(pictureIs(other, 5));

    cue.tick(6);
    TEST_ASSERT_EQ(cue.source->loads, 2);
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(first), 6));
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 6));
    TEST_ASSERT_TRUE(cue.cpu(second)->data() == cue.cpu(first)->data());
    TEST_ASSERT_TRUE(owned);
    return true;
}

// The engine unloads a cue's layers one after another: every unload of the current driver promotes the next.
bool test_SharedPromotion_ChainOfPromotions() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int first = cue.addSecondary(driver);
    const int second = cue.addSecondary(driver);
    cue.tick(5);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(driver));
    bool owned = ownsOrEmpty(cue.cpu(first));
#if !ROW_UNDER_ASAN
    TEST_ASSERT_TRUE(owned);
#endif
    cue.tick(6);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(first));
    TEST_ASSERT_TRUE(cue.manager.getLayer(second)->playback().isDecodeDriver());
    owned = owned && ownsOrEmpty(cue.cpu(second));
#if !ROW_UNDER_ASAN
    TEST_ASSERT_TRUE(owned);
#endif
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 6));
    cue.tick(7);
    TEST_ASSERT_EQ(cue.source->loads, 3);
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 7));

    TEST_ASSERT_TRUE(cue.manager.removeLayer(second));
    TEST_ASSERT_EQ(cue.manager.getLayerCount(), 0u);
    TEST_ASSERT_TRUE(owned);
    return true;
}

// The same two unloads with no render tick between them, as when both arrive in one drain of the OSC queue: the
// memory is handed on a second time by a layer that owned it without ever decoding into it.
bool test_SharedPromotion_BackToBackRemovalsWithoutATick() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int first = cue.addSecondary(driver);
    const int second = cue.addSecondary(driver);
    cue.tick(5);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(driver));
    TEST_ASSERT_TRUE(cue.manager.removeLayer(first));
    TEST_ASSERT_TRUE(cue.manager.getLayer(second)->playback().isDecodeDriver());
    const bool owned = ownsOrEmpty(cue.cpu(second));
#if !ROW_UNDER_ASAN
    TEST_ASSERT_TRUE(owned);
#endif
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 5));
    TEST_ASSERT_EQ(cue.source->loads, 1);

    cue.tick(6);
    TEST_ASSERT_EQ(cue.source->loads, 2);
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 6));
    TEST_ASSERT_TRUE(owned);
    return true;
}

// A promotion with no frame decoded yet (the driver is unloaded before the first tick): nothing to hand over,
// and the promoted layer decodes into memory of its own.
bool test_SharedPromotion_BeforeTheFirstFrame() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int secondary = cue.addSecondary(driver);

    TEST_ASSERT_TRUE(cue.manager.removeLayer(driver));
    TEST_ASSERT_TRUE(cue.manager.getLayer(secondary)->playback().isDecodeDriver());
    TEST_ASSERT_TRUE(ownsOrEmpty(cue.cpu(secondary)));
    cue.tick(5);
    TEST_ASSERT_EQ(cue.source->loads, 1);
    const FrameBuffer* buffer = cue.cpu(secondary);
    TEST_ASSERT_TRUE(buffer && buffer->isValid() && buffer->ownsBuffer());
    TEST_ASSERT_TRUE(pictureIs(buffer, 5));
    return true;
}

// Removing a secondary is not a promotion: the driver keeps its pixels and the other layers their views.
bool test_SharedPromotion_RemovingASecondaryChangesNothing() {
    SharedCue cue;
    const int driver = cue.addDriver();
    const int first = cue.addSecondary(driver);
    const int second = cue.addSecondary(driver);
    cue.tick(5);
    const uint8_t* pixels = cue.cpu(driver)->data();

    TEST_ASSERT_TRUE(cue.manager.removeLayer(first));
    TEST_ASSERT_TRUE(cue.manager.getLayer(driver)->playback().isDecodeDriver());
    TEST_ASSERT_FALSE(cue.manager.getLayer(second)->playback().isDecodeDriver());
    TEST_ASSERT_TRUE(cue.cpu(driver)->ownsBuffer());
    TEST_ASSERT_TRUE(cue.cpu(driver)->data() == pixels);
    TEST_ASSERT_TRUE(cue.cpu(second)->data() == pixels);

    cue.tick(6);
    TEST_ASSERT_EQ(cue.source->loads, 2);
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(driver), 6));
    TEST_ASSERT_TRUE(pictureIs(cue.cpu(second), 6));
    return true;
}
