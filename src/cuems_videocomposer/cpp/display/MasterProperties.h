#ifndef VIDEOCOMPOSER_MASTERPROPERTIES_H
#define VIDEOCOMPOSER_MASTERPROPERTIES_H

namespace videocomposer {

/**
 * MasterProperties - Transform properties for the composite output
 * 
 * These transforms are applied to the composite of all layers
 * before OSD is rendered. Similar to LayerProperties but for
 * the final output.
 */
struct MasterProperties {
    // Position offset (in normalized coordinates, -1.0 to 1.0)
    float x = 0.0f;
    float y = 0.0f;
    
    // Opacity (0.0 - 1.0)
    float opacity = 1.0f;
    
    // Scale (1.0 = 100%)
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    
    // Rotation in degrees
    float rotation = 0.0f;
    
    // Corner deformation (warping) - 4 corners, each with x,y offset
    // [corner1x, corner1y, corner2x, corner2y, corner3x, corner3y, corner4x, corner4y]
    // Offsets are in normalized coordinates (-1.0 to 1.0)
    struct {
        float corners[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        bool enabled = false;
    } cornerDeform;
    
    // Color adjustment controls
    struct ColorAdjustment {
        float brightness = 0.0f;   // -1.0 to 1.0 (0 = no change)
        float contrast = 1.0f;     // 0.0 to 2.0 (1 = no change)
        float saturation = 1.0f;   // 0.0 to 2.0 (1 = no change, 0 = grayscale)
        float hue = 0.0f;          // -180 to 180 degrees
        float gamma = 1.0f;        // 0.1 to 3.0 (1 = no change)
        
        // Check if any adjustments are active (non-default)
        bool isActive() const {
            return brightness != 0.0f || contrast != 1.0f || 
                   saturation != 1.0f || hue != 0.0f || gamma != 1.0f;
        }
        
        // Reset all to defaults
        void reset() {
            brightness = 0.0f;
            contrast = 1.0f;
            saturation = 1.0f;
            hue = 0.0f;
            gamma = 1.0f;
        }
    };
    ColorAdjustment colorAdjust;
    
    // Check if any transforms are active (non-default)
    // If all values are default, we can skip FBO and render directly
    bool isActive() const {
        // Position offset
        if (x != 0.0f || y != 0.0f) return true;
        
        // Opacity (not 1.0)
        if (opacity != 1.0f) return true;
        
        // Scale (not 1.0)
        if (scaleX != 1.0f || scaleY != 1.0f) return true;
        
        // Rotation
        if (rotation != 0.0f) return true;
        
        // Corner deformation
        if (cornerDeform.enabled) return true;
        
        // Color adjustments
        if (colorAdjust.isActive()) return true;
        
        return false;
    }
    
    // Reset all properties to defaults
    void reset() {
        x = 0.0f;
        y = 0.0f;
        opacity = 1.0f;
        scaleX = 1.0f;
        scaleY = 1.0f;
        rotation = 0.0f;
        cornerDeform.enabled = false;
        for (int i = 0; i < 8; i++) {
            cornerDeform.corners[i] = 0.0f;
        }
        colorAdjust.reset();
    }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MASTERPROPERTIES_H

