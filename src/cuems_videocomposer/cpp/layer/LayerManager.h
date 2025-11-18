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

