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
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include <memory>

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

bool test_LayerManager_AddLayer() {
    LayerManager manager;
    
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::make_unique<MockInputSource>());
    layer->setLayerId(1);
    
    int id = manager.addLayer(std::move(layer));
    TEST_ASSERT_EQ(id, 1);
    TEST_ASSERT_EQ(manager.getLayerCount(), 1);
    
    VideoLayer* retrieved = manager.getLayer(1);
    TEST_ASSERT_TRUE(retrieved != nullptr);
    TEST_ASSERT_EQ(retrieved->getLayerId(), 1);
    
    return true;
}

bool test_LayerManager_RemoveLayer() {
    LayerManager manager;
    
    auto layer1 = std::make_unique<VideoLayer>();
    layer1->setInputSource(std::make_unique<MockInputSource>());
    layer1->setLayerId(1);
    
    auto layer2 = std::make_unique<VideoLayer>();
    layer2->setInputSource(std::make_unique<MockInputSource>());
    layer2->setLayerId(2);
    
    manager.addLayer(std::move(layer1));
    manager.addLayer(std::move(layer2));
    
    TEST_ASSERT_EQ(manager.getLayerCount(), 2);
    
    bool removed = manager.removeLayer(1);
    TEST_ASSERT_TRUE(removed);
    TEST_ASSERT_EQ(manager.getLayerCount(), 1);
    TEST_ASSERT_TRUE(manager.getLayer(1) == nullptr);
    TEST_ASSERT_TRUE(manager.getLayer(2) != nullptr);
    
    return true;
}

bool test_LayerManager_ZOrder() {
    LayerManager manager;
    
    auto layer1 = std::make_unique<VideoLayer>();
    layer1->setInputSource(std::make_unique<MockInputSource>());
    layer1->properties().zOrder = 10;
    
    auto layer2 = std::make_unique<VideoLayer>();
    layer2->setInputSource(std::make_unique<MockInputSource>());
    layer2->properties().zOrder = 5;
    
    auto layer3 = std::make_unique<VideoLayer>();
    layer3->setInputSource(std::make_unique<MockInputSource>());
    layer3->properties().zOrder = 15;
    
    manager.addLayer(std::move(layer1));
    manager.addLayer(std::move(layer2));
    manager.addLayer(std::move(layer3));
    
    auto sorted = manager.getLayersSortedByZOrder();
    TEST_ASSERT_EQ(sorted.size(), 3);
    // After descending sort (highest zOrder first), order should be: 15, 10, 5
    TEST_ASSERT_EQ(sorted[0]->properties().zOrder, 15);
    TEST_ASSERT_EQ(sorted[1]->properties().zOrder, 10);
    TEST_ASSERT_EQ(sorted[2]->properties().zOrder, 5);
    
    return true;
}

bool test_LayerManager_DuplicateLayer() {
    LayerManager manager;
    
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::make_unique<MockInputSource>());
    layer->setLayerId(1);
    layer->properties().opacity = 0.5f;
    layer->properties().zOrder = 5;
    
    int id1 = manager.addLayer(std::move(layer));
    TEST_ASSERT_EQ(id1, 1);
    
    int id2 = manager.duplicateLayer(1);
    TEST_ASSERT_TRUE(id2 > 0);
    TEST_ASSERT_EQ(manager.getLayerCount(), 2);
    
    VideoLayer* original = manager.getLayer(1);
    VideoLayer* duplicate = manager.getLayer(id2);
    TEST_ASSERT_TRUE(original != nullptr);
    TEST_ASSERT_TRUE(duplicate != nullptr);
    TEST_ASSERT_EQ(duplicate->properties().opacity, 0.5f);
    TEST_ASSERT_EQ(duplicate->properties().zOrder, 5);
    
    return true;
}

bool test_LayerManager_Reorder() {
    LayerManager manager;
    
    auto layer1 = std::make_unique<VideoLayer>();
    layer1->setInputSource(std::make_unique<MockInputSource>());
    layer1->setLayerId(1);
    layer1->properties().zOrder = 1;
    
    auto layer2 = std::make_unique<VideoLayer>();
    layer2->setInputSource(std::make_unique<MockInputSource>());
    layer2->setLayerId(2);
    layer2->properties().zOrder = 2;
    
    int id1 = manager.addLayer(std::move(layer1));
    int id2 = manager.addLayer(std::move(layer2));
    
    // After addLayer, layers get new IDs assigned, so we need to use the returned IDs
    bool moved = manager.moveLayerToTop(id1);
    TEST_ASSERT_TRUE(moved);
    
    auto sorted = manager.getLayersSortedByZOrder();
    // After moveLayerToTop, layer with id1 should have highest zOrder and be first
    TEST_ASSERT_EQ(sorted[0]->getLayerId(), id1);
    TEST_ASSERT_EQ(sorted[1]->getLayerId(), id2);
    
    return true;
}


// ---------------------------------------------------------------------------
// resolveLayerAddress: the one way an OSC layer name becomes a layer (869f8j1ja)
// ---------------------------------------------------------------------------

// Returns the integer id the manager assigned, or -1 if the add failed.
static int addCueLayer(LayerManager& manager, const std::string& cueId) {
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::make_unique<MockInputSource>());
    VideoLayer* raw = layer.get();
    if (!manager.addLayerWithId(cueId, std::move(layer))) {
        return -1;
    }
    return raw->getLayerId();
}

bool test_LayerManager_ResolveLiveCueId() {
    LayerManager manager;
    int id = addCueLayer(manager, "a1b2c3d4-0000-4000-8000-000000000001_0");
    TEST_ASSERT_TRUE(id > 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("a1b2c3d4-0000-4000-8000-000000000001_0"), id);
    return true;
}

bool test_LayerManager_ResolveAbsentDigitUuidIsNotAnIntegerId() {
    // Integer ids restart at 1 on every /reset, so a show always has layers
    // 1, 2, ... atoi("2ac1fe93-...") is 2: the absent cue must resolve to
    // nothing, not to the live layer 2.
    LayerManager manager;
    addCueLayer(manager, "1111aaaa-0000-4000-8000-000000000001_0");
    int id2 = addCueLayer(manager, "2222bbbb-0000-4000-8000-000000000002_0");
    TEST_ASSERT_EQ(id2, 2);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("2ac1fe93-0000-4000-8000-000000000003_0"), 0);
    return true;
}

bool test_LayerManager_ResolveIntegerId() {
    LayerManager manager;
    addCueLayer(manager, "1111aaaa-0000-4000-8000-000000000001_0");
    int id2 = addCueLayer(manager, "2222bbbb-0000-4000-8000-000000000002_0");
    TEST_ASSERT_EQ(manager.resolveLayerAddress("2"), id2);
    return true;
}

bool test_LayerManager_ResolveMalformedOrMissing() {
    LayerManager manager;
    addCueLayer(manager, "1111aaaa-0000-4000-8000-000000000001_0");
    addCueLayer(manager, "2222bbbb-0000-4000-8000-000000000002_0");
    TEST_ASSERT_EQ(manager.resolveLayerAddress("0"), 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress(""), 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("2abc"), 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("1234567890"), 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("7"), 0);
    return true;
}

bool test_LayerManager_ResolveCueIdWinsOverIntegerId() {
    // A cue id that reads as an integer resolves as the cue id: the lookup
    // order (cue id first, then integer id) is part of the contract.
    LayerManager manager;
    int first = addCueLayer(manager, "5");   // integer id 1, cue id "5"
    addCueLayer(manager, "other_0");         // integer id 2
    TEST_ASSERT_EQ(first, 1);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("5"), first);
    int third = addCueLayer(manager, "2");   // integer id 3, cue id "2"
    TEST_ASSERT_EQ(manager.resolveLayerAddress("2"), third);
    return true;
}

bool test_LayerManager_ResolveAfterRemovalIsGone() {
    LayerManager manager;
    addCueLayer(manager, "1111aaaa-0000-4000-8000-000000000001_0");
    TEST_ASSERT_TRUE(manager.removeLayerByCueId("1111aaaa-0000-4000-8000-000000000001_0"));
    TEST_ASSERT_EQ(manager.resolveLayerAddress("1111aaaa-0000-4000-8000-000000000001_0"), 0);
    TEST_ASSERT_EQ(manager.resolveLayerAddress("1"), 0);
    return true;
}
