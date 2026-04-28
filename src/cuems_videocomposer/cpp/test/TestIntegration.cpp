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

#include "TestFramework.h"
#include "../layer/LayerManager.h"
#include "../layer/VideoLayer.h"
#include "../config/ConfigurationManager.h"
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../sync/MIDISyncSource.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace videocomposer;
using namespace videocomposer::test;

// Mock InputSource for testing
class MockInputSource : public InputSource {
public:
    bool open(const std::string& source) override { return true; }
    void close() override {}
    bool seek(int64_t frameNumber) override { return true; }
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override { return true; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 1920;
        info.height = 1080;
        info.framerate = 25.0;
        info.totalFrames = 1000;
        return info;
    }
    bool isReady() const override { return true; }
    int64_t getCurrentFrame() const override { return 0; }
    
    // New methods from InputSource interface
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
};

bool test_Integration_LayerManagerWithMultipleLayers() {
    LayerManager manager;
    
    // Create multiple layers
    for (int i = 0; i < 5; ++i) {
        auto layer = std::make_unique<VideoLayer>();
        layer->setInputSource(std::make_unique<MockInputSource>());
        layer->setLayerId(i + 1);
        layer->properties().zOrder = i;
        layer->properties().opacity = 0.5f + (i * 0.1f);
        
        int id = manager.addLayer(std::move(layer));
        TEST_ASSERT_EQ(id, i + 1);
    }
    
    TEST_ASSERT_EQ(manager.getLayerCount(), 5);
    
    // Test z-order sorting (descending - highest zOrder first)
    auto sorted = manager.getLayersSortedByZOrder();
    TEST_ASSERT_EQ(sorted.size(), 5);
    // After descending sort, order should be: 4, 3, 2, 1, 0 (highest first)
    for (size_t i = 0; i < sorted.size(); ++i) {
        TEST_ASSERT_EQ(sorted[i]->properties().zOrder, static_cast<int>(sorted.size() - 1 - i));
    }
    
    // Test layer removal
    bool removed = manager.removeLayer(3);
    TEST_ASSERT_TRUE(removed);
    TEST_ASSERT_EQ(manager.getLayerCount(), 4);
    
    return true;
}

bool test_Integration_VideoLayerTimeScaling() {
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::make_unique<MockInputSource>());
    
    // Test time-scaling combination
    layer->setTimeScale(2.0);
    layer->setTimeOffset(100);
    layer->setWraparound(true);
    
    TEST_ASSERT_EQ(layer->getTimeScale(), 2.0);
    TEST_ASSERT_EQ(layer->getTimeOffset(), 100);
    TEST_ASSERT_TRUE(layer->getWraparound());
    
    // Test reverse
    layer->reverse();
    TEST_ASSERT_EQ(layer->getTimeScale(), -2.0);
    
    return true;
}

bool test_Integration_LayerProperties() {
    auto layer = std::make_unique<VideoLayer>();
    auto& props = layer->properties();
    
    // Test all property types
    props.x = 100;
    props.y = 200;
    props.width = 1920;
    props.height = 1080;
    props.opacity = 0.75f;
    props.zOrder = 5;
    props.visible = true;
    props.scaleX = 1.5f;
    props.scaleY = 0.8f;
    props.rotation = 45.0f;
    
    TEST_ASSERT_EQ(props.x, 100);
    TEST_ASSERT_EQ(props.y, 200);
    TEST_ASSERT_EQ(props.width, 1920);
    TEST_ASSERT_EQ(props.height, 1080);
    TEST_ASSERT_EQ(props.opacity, 0.75f);
    TEST_ASSERT_EQ(props.zOrder, 5);
    TEST_ASSERT_TRUE(props.visible);
    TEST_ASSERT_EQ(props.scaleX, 1.5f);
    TEST_ASSERT_EQ(props.scaleY, 0.8f);
    TEST_ASSERT_EQ(props.rotation, 45.0f);
    
    // Test crop
    props.crop.enabled = true;
    props.crop.x = 100;
    props.crop.y = 50;
    props.crop.width = 800;
    props.crop.height = 600;
    
    TEST_ASSERT_TRUE(props.crop.enabled);
    TEST_ASSERT_EQ(props.crop.x, 100);
    TEST_ASSERT_EQ(props.crop.y, 50);
    TEST_ASSERT_EQ(props.crop.width, 800);
    TEST_ASSERT_EQ(props.crop.height, 600);
    
    // Test panorama mode
    props.panoramaMode = true;
    props.panOffset = 500;

    TEST_ASSERT_TRUE(props.panoramaMode);
    TEST_ASSERT_EQ(props.panOffset, 500);

    return true;
}

// Atomicity smoke test: setDisplayLatencyMs from one thread while getTimeMs
// runs from another. The displayLatencyMs_ field is std::atomic<long>, so
// reads must always observe a coherent value (one of the writes — never a
// torn or out-of-range value). The point isn't to verify scheduling order
// but to catch any future regression that drops the atomic qualifier or
// adds non-atomic state alongside it.
bool test_Integration_MIDISyncSource_DisplayLatencyAtomicity() {
    MIDISyncSource sync;
    std::atomic<bool> stop{false};
    std::atomic<long> mismatches{0};

    std::thread writer([&]() {
        long values[] = {0, 33, 50, 71, 90, 120};
        size_t i = 0;
        while (!stop.load()) {
            sync.setDisplayLatencyMs(values[i % 6]);
            ++i;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        // No cue is loaded ⇒ getTimeMs returns wire-MTC + compensation. We
        // can't predict the exact return value, but it must be in the clamped
        // [-10000, 10_000_000] range — anything wildly outside indicates a torn read.
        long t = sync.getTimeMs();
        if (t < -10000 || t > 10000000) {
            ++mismatches;
        }
    }
    stop.store(true);
    writer.join();

    TEST_ASSERT_EQ(mismatches.load(), 0L);
    return true;
}

