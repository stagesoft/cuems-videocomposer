#ifndef VIDEOCOMPOSER_LAYERPROPERTIES_H
#define VIDEOCOMPOSER_LAYERPROPERTIES_H

#include <cstdint>

namespace videocomposer {

/**
 * LayerProperties - Display properties for a video layer
 */
struct LayerProperties {
    int x = 0;              // Position X
    int y = 0;              // Position Y
    int width = 0;          // Width
    int height = 0;         // Height
    float opacity = 1.0f;   // Opacity (0.0 - 1.0)
    int zOrder = 0;         // Layer stacking order
    bool visible = true;    // Show/hide layer
    
    // Transform (simplified for now, can be extended)
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float rotation = 0.0f;
    
    // Crop rectangle
    struct {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool enabled = false;  // Enable cropping
    } crop;
    
    // Panorama mode (50% width crop with x-offset for panning)
    bool panoramaMode = false;  // Enable panorama mode (crops to 50% width)
    int panOffset = 0;          // X-offset for panning in panorama mode (0 to video width)
    
    // Blend mode (simplified enum for now)
    enum BlendMode {
        NORMAL = 0,
        MULTIPLY,
        SCREEN,
        OVERLAY
    };
    BlendMode blendMode = NORMAL;
    
    // Corner deformation (warping) - 4 corners, each with x,y offset
    struct {
        float corners[8];  // [corner1x, corner1y, corner2x, corner2y, corner3x, corner3y, corner4x, corner4y]
        bool enabled = false;
    } cornerDeform;
    
    // Auto-unload: automatically unload file when playback ends
    bool autoUnload = false;
    
    // Full file loop count (for wraparound)
    int fullFileLoopCount = -1;        // -1 = infinite, 0 = no loop, >0 = loop N times
    int currentFullFileLoopCount = -1; // Current loop iteration (starts at fullFileLoopCount, decrements)
    
    // Loop region: loop a specific region of the file
    struct LoopRegion {
        bool enabled = false;
        int64_t startFrame = 0;
        int64_t endFrame = 0;
        int loopCount = -1;        // -1 = infinite, 0 = no loop, >0 = loop N times
        int currentLoopCount = -1; // Current loop iteration (starts at loopCount, decrements)
    };
    LoopRegion loopRegion;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERPROPERTIES_H

