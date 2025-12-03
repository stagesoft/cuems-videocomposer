/**
 * OutputRegion.h - Output region configuration for Virtual Canvas
 * 
 * Defines where an output sits in the virtual canvas and its
 * blend/warp configuration for projection mapping.
 */

#ifndef VIDEOCOMPOSER_OUTPUTREGION_H
#define VIDEOCOMPOSER_OUTPUTREGION_H

#include <string>
#include <memory>

namespace videocomposer {

/**
 * BlendEdges - Edge blending configuration for projector overlap
 * 
 * Specifies the width of the soft-edge blend zone on each edge.
 * Values are in pixels. 0 = no blending on that edge.
 */
struct BlendEdges {
    float left = 0.0f;      // Left edge blend width in pixels
    float right = 0.0f;     // Right edge blend width in pixels
    float top = 0.0f;       // Top edge blend width in pixels
    float bottom = 0.0f;    // Bottom edge blend width in pixels
    float gamma = 2.2f;     // Gamma correction for perceptual blending
    
    bool hasBlending() const {
        return left > 0.0f || right > 0.0f || top > 0.0f || bottom > 0.0f;
    }
    
    void reset() {
        left = right = top = bottom = 0.0f;
        gamma = 2.2f;
    }
};

// Forward declaration
class WarpMesh;

/**
 * OutputRegion - Defines an output's position and configuration in the virtual canvas
 * 
 * The virtual canvas is a large FBO that contains all outputs.
 * Each OutputRegion specifies:
 * - Where in the canvas this output's content comes from (canvasX/Y)
 * - The size of the region (which may differ from physical output size for scaling)
 * - Edge blending configuration
 * - Optional warp mesh for geometric correction
 */
struct OutputRegion {
    // ===== Identification =====
    std::string name;               // Output name: "HDMI-A-1", "DP-1", etc.
    int index = -1;                 // Index in output list
    
    // ===== Canvas Position =====
    // Where this output's content comes from in the virtual canvas
    int canvasX = 0;                // X position in virtual canvas (pixels)
    int canvasY = 0;                // Y position in virtual canvas (pixels)
    int canvasWidth = 0;            // Width in canvas (pixels)
    int canvasHeight = 0;           // Height in canvas (pixels)
    
    // ===== Physical Output =====
    // The actual output resolution (may differ from canvas region for scaling)
    int physicalWidth = 0;          // Physical output width
    int physicalHeight = 0;         // Physical output height
    
    // ===== Blending =====
    BlendEdges blend;               // Edge blending configuration
    
    // ===== Warping =====
    std::shared_ptr<WarpMesh> warpMesh;  // Optional warp mesh for geometric correction
    std::string warpMeshPath;       // Path to warp mesh file (for persistence)
    
    // ===== State =====
    bool enabled = true;            // Is this output active?
    
    // ===== Helper Methods =====
    
    /**
     * Check if this region has edge blending enabled
     */
    bool hasBlending() const {
        return blend.hasBlending();
    }
    
    /**
     * Check if this region has warping enabled
     */
    bool hasWarping() const {
        return warpMesh != nullptr;
    }
    
    /**
     * Get the canvas rect as floats (for shader uniforms)
     * Returns: x, y, width, height in canvas pixels
     */
    void getCanvasRect(float& x, float& y, float& w, float& h) const {
        x = static_cast<float>(canvasX);
        y = static_cast<float>(canvasY);
        w = static_cast<float>(canvasWidth);
        h = static_cast<float>(canvasHeight);
    }
    
    /**
     * Check if canvas region matches physical size (no scaling)
     */
    bool isOneToOne() const {
        return canvasWidth == physicalWidth && canvasHeight == physicalHeight;
    }
    
    /**
     * Get scale factor (canvas to physical)
     */
    float getScaleX() const {
        return canvasWidth > 0 ? static_cast<float>(physicalWidth) / canvasWidth : 1.0f;
    }
    
    float getScaleY() const {
        return canvasHeight > 0 ? static_cast<float>(physicalHeight) / canvasHeight : 1.0f;
    }
    
    /**
     * Create a default 1:1 region for an output
     */
    static OutputRegion createDefault(const std::string& outputName, int index,
                                      int width, int height, int canvasX = 0, int canvasY = 0) {
        OutputRegion region;
        region.name = outputName;
        region.index = index;
        region.canvasX = canvasX;
        region.canvasY = canvasY;
        region.canvasWidth = width;
        region.canvasHeight = height;
        region.physicalWidth = width;
        region.physicalHeight = height;
        region.enabled = true;
        return region;
    }
};

/**
 * WarpMesh - Placeholder for geometric warp mesh
 * 
 * TODO: Implement mesh loading and GPU representation
 * Supports common mesh formats for keystone/geometric correction.
 */
class WarpMesh {
public:
    WarpMesh() = default;
    virtual ~WarpMesh() = default;
    
    /**
     * Load mesh from file
     * @param path Path to mesh file (.obj, .mesh, etc.)
     * @return true on success
     */
    virtual bool loadFromFile(const std::string& path) { 
        (void)path;
        return false; 
    }
    
    /**
     * Get mesh GPU texture (displacement map)
     * @return OpenGL texture ID, or 0 if not loaded
     */
    virtual unsigned int getTexture() const { return 0; }
    
    /**
     * Check if mesh is loaded and valid
     */
    virtual bool isValid() const { return false; }
    
    /**
     * Get mesh dimensions
     */
    virtual void getDimensions(int& width, int& height) const {
        width = 0;
        height = 0;
    }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTREGION_H

