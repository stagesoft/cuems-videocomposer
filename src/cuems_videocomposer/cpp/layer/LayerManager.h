/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_LAYERMANAGER_H
#define VIDEOCOMPOSER_LAYERMANAGER_H

#include "VideoLayer.h"
#include <vector>
#include <memory>
#include <map>
#include <string>
#include <cstdint>

namespace videocomposer {

/**
 * LayerManager - Manages collection of VideoLayer instances
 * 
 * Handles layer ordering, compositing, and provides interface
 * for adding/removing layers.
 */
class LayerManager {
public:
    LayerManager();
    ~LayerManager();

    // Layer management
    int addLayer(std::unique_ptr<VideoLayer> layer);
    bool removeLayer(int layerId);
    VideoLayer* getLayer(int layerId);
    const VideoLayer* getLayer(int layerId) const;
    
    // Remove all layers (atomic reset for project load)
    void removeAllLayers();

    // Layer management by UUID (cue ID)
    bool addLayerWithId(const std::string& cueId, std::unique_ptr<VideoLayer> layer);
    bool removeLayerByCueId(const std::string& cueId);
    VideoLayer* getLayerByCueId(const std::string& cueId);
    const VideoLayer* getLayerByCueId(const std::string& cueId) const;
    std::string getCueIdFromLayer(VideoLayer* layer) const;
    
    // Get all layers (sorted by z-order)
    std::vector<VideoLayer*> getLayers();
    std::vector<const VideoLayer*> getLayers() const;
    
    // Layer count
    size_t getLayerCount() const { return layers_.size(); }
    
    // Update all layers
    void updateAll();
    
    // Get layer by index
    VideoLayer* getLayerByIndex(size_t index);
    const VideoLayer* getLayerByIndex(size_t index) const;

    // Layer manipulation
    bool setLayerZOrder(int layerId, int zOrder);
    bool duplicateLayer(int layerId, int* newLayerId = nullptr);
    bool moveLayerToTop(int layerId);
    bool moveLayerToBottom(int layerId);
    bool moveLayerUp(int layerId);
    bool moveLayerDown(int layerId);
    
    // Get layers sorted by z-order (for rendering)
    std::vector<VideoLayer*> getLayersSortedByZOrder();
    std::vector<const VideoLayer*> getLayersSortedByZOrder() const;

private:
    std::vector<std::unique_ptr<VideoLayer>> layers_;
    int nextLayerId_;
    std::map<std::string, int> cueIdToLayerId_;  // Map UUID cue ID to internal layer ID
    
    void sortLayersByZOrder();
    int getNextZOrder();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERMANAGER_H

