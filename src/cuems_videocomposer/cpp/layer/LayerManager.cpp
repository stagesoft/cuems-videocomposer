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

#include "LayerManager.h"
#include <chrono>
#include "../utils/Logger.h"
#include <algorithm>
#include <map>

namespace videocomposer {

LayerManager::LayerManager()
    : nextLayerId_(1)
{
}

LayerManager::~LayerManager() {
    layers_.clear();
}

void LayerManager::removeAllLayers() {
    layers_.clear();
    cueIdToLayerId_.clear();
    nextLayerId_ = 1;
}

int LayerManager::addLayer(std::unique_ptr<VideoLayer> layer) {
    if (!layer) {
        return -1;
    }
    
    int layerId = nextLayerId_++;
    layer->setLayerId(layerId);
    layers_.push_back(std::move(layer));
    
    sortLayersByZOrder();
    return layerId;
}

bool LayerManager::removeLayer(int layerId) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [layerId](const std::unique_ptr<VideoLayer>& layer) {
            return layer->getLayerId() == layerId;
        });

    if (it != layers_.end()) {
        // Promote a secondary to decode driver before removing (if this was the driver)
        promoteDecodeDriver(it->get());

        // Remove from cueIdToLayerId_ map if present
        for (auto mapIt = cueIdToLayerId_.begin(); mapIt != cueIdToLayerId_.end(); ++mapIt) {
            if (mapIt->second == layerId) {
                cueIdToLayerId_.erase(mapIt);
                break;
            }
        }
        layers_.erase(it);
        return true;
    }

    return false;
}

VideoLayer* LayerManager::getLayer(int layerId) {
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [layerId](const std::unique_ptr<VideoLayer>& layer) {
            return layer->getLayerId() == layerId;
        });
    
    if (it != layers_.end()) {
        return it->get();
    }
    
    return nullptr;
}

const VideoLayer* LayerManager::getLayer(int layerId) const {
    auto it = std::find_if(layers_.begin(), layers_.end(),
        [layerId](const std::unique_ptr<VideoLayer>& layer) {
            return layer->getLayerId() == layerId;
        });
    
    if (it != layers_.end()) {
        return it->get();
    }
    
    return nullptr;
}

std::vector<VideoLayer*> LayerManager::getLayers() {
    std::vector<VideoLayer*> result;
    result.reserve(layers_.size());
    
    for (auto& layer : layers_) {
        result.push_back(layer.get());
    }
    
    return result;
}

std::vector<const VideoLayer*> LayerManager::getLayers() const {
    std::vector<const VideoLayer*> result;
    result.reserve(layers_.size());
    
    for (const auto& layer : layers_) {
        result.push_back(layer.get());
    }
    
    return result;
}

void LayerManager::updateAll() {
    // INVARIANT: updateAll() must complete for ALL layers before render() begins.
    // Shared-layer texture safety depends on this. If parallelizing decode,
    // ensure all decode jobs complete (waitAll) before returning from updateAll().

    // TODO: PERFORMANCE - Layer-parallel HAP decoding
    // Currently layers are updated sequentially, which means HAP decode happens
    // one layer at a time. For 10+ HAP layers, this takes 10-12ms (30-36% of frame budget).
    //
    // To achieve 6-7x speedup for multi-layer HAP:
    // 1. Create a shared DecodeThreadPool (singleton, hardware_concurrency threads)
    // 2. Submit all layer decode jobs to the pool in parallel
    // 3. Wait for all decodes to complete before rendering
    //
    // Example architecture:
    //   DecodeThreadPool& pool = DecodeThreadPool::instance();
    //   for (auto& layer : layers_) {
    //       pool.enqueue([&layer]() { layer->decodeFrame(); });
    //   }
    //   pool.waitAll();
    //   for (auto& layer : layers_) {
    //       layer->uploadToGPU();  // GPU uploads must be sequential (OpenGL context)
    //   }
    //
    // Note: GPU texture uploads must remain on main thread (OpenGL context bound).
    // Only the CPU decode (Snappy decompression) should be parallelized.
    // INVARIANT: If parallelizing, ensure all decode jobs complete before returning.

    // Build update-order view sorted by updatePriority_ (drivers before secondaries).
    // This is decoupled from z-order: layers_ remains sorted by z-order for rendering.
    std::vector<VideoLayer*> updateOrder;
    updateOrder.reserve(layers_.size());
    for (auto& layer : layers_) {
        if (layer && layer->isReady()) {
            updateOrder.push_back(layer.get());
        }
    }
    std::stable_sort(updateOrder.begin(), updateOrder.end(),
        [](const VideoLayer* a, const VideoLayer* b) {
            return a->getUpdatePriority() < b->getUpdatePriority();
        });

    // Collect layers to remove (auto-unload)
    std::vector<int> layersToRemove;

    for (auto* layer : updateOrder) {
        layer->update();

        // Check for auto-unload: if playback ended and autoUnload is enabled
        auto& props = layer->properties();
        if (props.autoUnload && layer->getInputSource()) {
            // Check if playback has ended (no loop, at end of file)
            FrameInfo info = layer->getFrameInfo();
            int64_t currentFrame = layer->getCurrentFrame();
            int64_t totalFrames = info.totalFrames;

            // Playback has ended if:
            // 1. Current frame is at or beyond total frames
            // 2. No wraparound (full file loop) is enabled, OR wraparound is enabled but loop count reached
            // 3. No region loop is enabled
            bool wraparoundActive = layer->getWraparound() &&
                                   (props.fullFileLoopCount == -1 || props.currentFullFileLoopCount > 0);
            if (currentFrame >= totalFrames &&
                !wraparoundActive &&
                !props.loopRegion.enabled) {
                // Mark layer for removal
                layersToRemove.push_back(layer->getLayerId());
            }
        }
    }

    // Remove layers marked for auto-unload
    for (int layerId : layersToRemove) {
        removeLayer(layerId);
    }
}

VideoLayer* LayerManager::getLayerByIndex(size_t index) {
    if (index < layers_.size()) {
        return layers_[index].get();
    }
    return nullptr;
}

const VideoLayer* LayerManager::getLayerByIndex(size_t index) const {
    if (index < layers_.size()) {
        return layers_[index].get();
    }
    return nullptr;
}

void LayerManager::sortLayersByZOrder() {
    std::sort(layers_.begin(), layers_.end(),
        [](const std::unique_ptr<VideoLayer>& a, const std::unique_ptr<VideoLayer>& b) {
            return a->properties().zOrder < b->properties().zOrder;
        });
}

int LayerManager::getNextZOrder() {
    if (layers_.empty()) {
        return 0;
    }
    int maxZOrder = layers_[0]->properties().zOrder;
    for (const auto& layer : layers_) {
        if (layer->properties().zOrder > maxZOrder) {
            maxZOrder = layer->properties().zOrder;
        }
    }
    return maxZOrder + 1;
}

bool LayerManager::setLayerZOrder(int layerId, int zOrder) {
    VideoLayer* layer = getLayer(layerId);
    if (!layer) {
        return false;
    }
    
    layer->properties().zOrder = zOrder;
    sortLayersByZOrder();
    return true;
}

bool LayerManager::duplicateLayer(int layerId, int* newLayerId) {
    VideoLayer* sourceLayer = getLayer(layerId);
    if (!sourceLayer) {
        return false;
    }

    // Create new layer with same input source
    // Note: This is a simplified duplication - we'd need to clone the input source
    // For now, we'll just create a new layer with the same properties
    // Full implementation would require cloning InputSource and SyncSource
    
    auto newLayer = std::make_unique<VideoLayer>();
    
    // Copy properties
    auto& newProps = newLayer->properties();
    const auto& sourceProps = sourceLayer->properties();
    newProps.x = sourceProps.x + 20; // Offset slightly
    newProps.y = sourceProps.y + 20;
    newProps.width = sourceProps.width;
    newProps.height = sourceProps.height;
    newProps.opacity = sourceProps.opacity;
    newProps.zOrder = getNextZOrder();
    newProps.visible = sourceProps.visible;
    newProps.scaleX = sourceProps.scaleX;
    newProps.scaleY = sourceProps.scaleY;
    newProps.rotation = sourceProps.rotation;
    newProps.blendMode = sourceProps.blendMode;

    // Note: We can't duplicate the input source easily without cloning
    // This would require InputSource to support cloning
    // For now, the duplicated layer won't have an input source
    
    int id = addLayer(std::move(newLayer));
    if (newLayerId) {
        *newLayerId = id;
    }
    return id >= 0;
}

bool LayerManager::moveLayerToTop(int layerId) {
    VideoLayer* layer = getLayer(layerId);
    if (!layer) {
        return false;
    }
    
    int maxZOrder = getNextZOrder() - 1;
    layer->properties().zOrder = maxZOrder + 1;
    sortLayersByZOrder();
    return true;
}

bool LayerManager::moveLayerToBottom(int layerId) {
    VideoLayer* layer = getLayer(layerId);
    if (!layer) {
        return false;
    }
    
    int minZOrder = 0;
    if (!layers_.empty()) {
        minZOrder = layers_[0]->properties().zOrder;
        for (const auto& l : layers_) {
            if (l->properties().zOrder < minZOrder) {
                minZOrder = l->properties().zOrder;
            }
        }
    }
    
    layer->properties().zOrder = minZOrder - 1;
    sortLayersByZOrder();
    return true;
}

bool LayerManager::moveLayerUp(int layerId) {
    VideoLayer* layer = getLayer(layerId);
    if (!layer) {
        return false;
    }
    
    int currentZOrder = layer->properties().zOrder;
    int nextZOrder = currentZOrder + 1;
    
    // Find the next highest z-order
    for (const auto& l : layers_) {
        if (l->getLayerId() != layerId && l->properties().zOrder > currentZOrder) {
            if (l->properties().zOrder < nextZOrder || nextZOrder == currentZOrder + 1) {
                nextZOrder = l->properties().zOrder + 1;
            }
        }
    }
    
    layer->properties().zOrder = nextZOrder;
    sortLayersByZOrder();
    return true;
}

bool LayerManager::moveLayerDown(int layerId) {
    VideoLayer* layer = getLayer(layerId);
    if (!layer) {
        return false;
    }
    
    int currentZOrder = layer->properties().zOrder;
    int nextZOrder = currentZOrder - 1;
    
    // Find the next lowest z-order
    for (const auto& l : layers_) {
        if (l->getLayerId() != layerId && l->properties().zOrder < currentZOrder) {
            if (l->properties().zOrder > nextZOrder || nextZOrder == currentZOrder - 1) {
                nextZOrder = l->properties().zOrder - 1;
            }
        }
    }
    
    layer->properties().zOrder = nextZOrder;
    sortLayersByZOrder();
    return true;
}

std::vector<VideoLayer*> LayerManager::getLayersSortedByZOrder() {
    std::vector<VideoLayer*> result;
    result.reserve(layers_.size());
    
    for (auto& layer : layers_) {
        result.push_back(layer.get());
    }
    
    // Sort by z-order (descending - higher zOrder first, so "top" layers are first)
    std::sort(result.begin(), result.end(),
        [](VideoLayer* a, VideoLayer* b) {
            return a->properties().zOrder > b->properties().zOrder;
        });
    
    return result;
}

std::vector<const VideoLayer*> LayerManager::getLayersSortedByZOrder() const {
    std::vector<const VideoLayer*> result;
    result.reserve(layers_.size());
    
    for (const auto& layer : layers_) {
        result.push_back(layer.get());
    }
    
    // Sort by z-order (descending - higher zOrder first, so "top" layers are first)
    std::sort(result.begin(), result.end(),
        [](const VideoLayer* a, const VideoLayer* b) {
            return a->properties().zOrder > b->properties().zOrder;
        });
    
    return result;
}

void LayerManager::promoteDecodeDriver(VideoLayer* removedLayer) {
    if (!removedLayer) return;

    // Only act if the removed layer was a decode driver
    if (!removedLayer->playback().isDecodeDriver()) return;

    InputSource* sharedInput = removedLayer->getInputSource();
    if (!sharedInput) return;

    // Invalidate cache — prevents secondaries from reading stale non-owning views
    // that point to the removed driver's freed frame buffers
    sharedInput->invalidateCache();

    // Find a surviving layer sharing the same InputSource and promote it
    for (auto& layer : layers_) {
        if (!layer || layer.get() == removedLayer) continue;
        if (layer->playback().getSharedInputSource().get() == sharedInput) {
            layer->playback().setDecodeDriver(true);
            layer->setUpdatePriority(0);
            // The decode session survives the driver that owned it, so its
            // hang-guard slot moves to the new owner instead of being dropped
            // and re-requested - which could otherwise be refused, stopping a
            // cue that never stopped decoding.
            layer->inheritGuardReservation(*removedLayer);
            LOG_INFO << "Promoted layer " << layer->getLayerId() << " to decode driver";
            return;
        }
    }
    // No surviving shared layers — InputSource will be freed by refcount when last shared_ptr is reset
}

bool LayerManager::addLayerWithId(const std::string& cueId, std::unique_ptr<VideoLayer> layer) {
    if (!layer) {
        return false;
    }
    
    int layerId = nextLayerId_++;
    layer->setLayerId(layerId);
    layer->setCueId(cueId);
    cueIdToLayerId_[cueId] = layerId;
    layers_.push_back(std::move(layer));
    
    sortLayersByZOrder();
    return true;
}

bool LayerManager::removeLayerByCueId(const std::string& cueId) {
    auto mapIt = cueIdToLayerId_.find(cueId);
    if (mapIt != cueIdToLayerId_.end()) {
        int layerId = mapIt->second;
        cueIdToLayerId_.erase(mapIt);
        return removeLayer(layerId);
    }
    return false;
}

VideoLayer* LayerManager::getLayerByCueId(const std::string& cueId) {
    auto mapIt = cueIdToLayerId_.find(cueId);
    if (mapIt != cueIdToLayerId_.end()) {
        return getLayer(mapIt->second);
    }
    return nullptr;
}

const VideoLayer* LayerManager::getLayerByCueId(const std::string& cueId) const {
    auto mapIt = cueIdToLayerId_.find(cueId);
    if (mapIt != cueIdToLayerId_.end()) {
        return getLayer(mapIt->second);
    }
    return nullptr;
}

std::string LayerManager::getCueIdFromLayer(VideoLayer* layer) const {
    if (!layer) {
        return "";
    }
    
    int layerId = layer->getLayerId();
    
    // Search map for cue ID that maps to this layer ID
    for (const auto& pair : cueIdToLayerId_) {
        if (pair.second == layerId) {
            return pair.first;
        }
    }
    
    return ""; // Not found
}

// ---------------------------------------------------------------------------
// Load outcomes
// ---------------------------------------------------------------------------

void LayerManager::recordLoadOutcome(const std::string& cueId,
                                     LoadOutcome::Result result,
                                     const std::string& reason) {
    if (cueId.empty()) {
        return;
    }
    LoadOutcome o;
    o.result = result;
    o.reason = reason;
    o.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(outcomeMutex_);
    loadOutcomes_[cueId] = o;
}

bool LayerManager::getLoadOutcome(const std::string& cueId, LoadOutcome& out) const {
    std::lock_guard<std::mutex> lock(outcomeMutex_);
    auto it = loadOutcomes_.find(cueId);
    if (it == loadOutcomes_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

std::map<std::string, LoadOutcome> LayerManager::getLoadOutcomes() const {
    std::lock_guard<std::mutex> lock(outcomeMutex_);
    return loadOutcomes_;
}

} // namespace videocomposer

