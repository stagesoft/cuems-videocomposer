/**
 * OutputConfig.h - Display output configuration structures
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Defines configuration structures for per-output settings including
 * layer routing, edge blending, and geometric warping.
 */

#ifndef VIDEOCOMPOSER_OUTPUTCONFIG_H
#define VIDEOCOMPOSER_OUTPUTCONFIG_H

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace videocomposer {

/**
 * BlendRegion - Edge blending configuration for projector setups
 * 
 * Defines soft-edge blending regions for overlapping projectors.
 * Values are normalized (0.0 - 0.5 typical) representing the
 * percentage of the output width/height used for blending.
 */
struct BlendRegion {
    float left = 0.0f;      // Left edge blend width (0.0 - 0.5)
    float right = 0.0f;     // Right edge blend width (0.0 - 0.5)
    float top = 0.0f;       // Top edge blend height (0.0 - 0.5)
    float bottom = 0.0f;    // Bottom edge blend height (0.0 - 0.5)
    float gamma = 2.2f;     // Gamma correction for blend curve
    
    /**
     * Check if any blending is configured
     */
    bool isEnabled() const {
        return left > 0.0f || right > 0.0f || top > 0.0f || bottom > 0.0f;
    }
    
    /**
     * Reset all blend values to defaults
     */
    void reset() {
        left = right = top = bottom = 0.0f;
        gamma = 2.2f;
    }
};

/**
 * OutputConfig - Per-output configuration
 * 
 * Configuration for a single display output including:
 * - Position and resolution
 * - Layer routing (which layers display on this output)
 * - Edge blending for projector setups
 * - Geometric warping/correction
 */
struct OutputConfig {
    // ===== Identification =====
    std::string outputName;     // Match to OutputInfo::name
    
    // ===== Position and Mode =====
    int32_t x = 0;              // Desired X position
    int32_t y = 0;              // Desired Y position
    int32_t width = 0;          // Desired resolution width (0 = use current)
    int32_t height = 0;         // Desired resolution height (0 = use current)
    double refreshRate = 0.0;   // Desired refresh rate (0 = use current)
    bool enabled = true;        // Enable/disable this output
    int32_t rotation = 0;       // Rotation: 0, 90, 180, 270 degrees
    
    // ===== Layer Routing =====
    // Which layers are routed to this output.
    // Empty vector means all layers (default behavior).
    // Use -1 as a special value to indicate "all layers".
    std::vector<int> assignedLayers;
    
    // ===== Edge Blending =====
    BlendRegion blend;
    bool blendEnabled = false;
    
    // ===== Geometric Warping =====
    bool warpEnabled = false;
    std::string warpMeshPath;   // Path to warp mesh file
    
    // ===== NDI/Virtual Output Capture =====
    bool captureForNDI = false;  // Capture this output for NDI streaming
    
    // ===== Helper Methods =====
    
    /**
     * Check if specific layer is assigned to this output
     */
    bool hasLayer(int layerId) const {
        if (assignedLayers.empty()) {
            return true;  // No specific assignment = all layers
        }
        for (int id : assignedLayers) {
            if (id == -1 || id == layerId) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * Assign a layer to this output
     */
    void addLayer(int layerId) {
        // Remove "all layers" marker if we're adding specific layers
        assignedLayers.erase(
            std::remove(assignedLayers.begin(), assignedLayers.end(), -1),
            assignedLayers.end()
        );
        // Add if not already present
        for (int id : assignedLayers) {
            if (id == layerId) return;
        }
        assignedLayers.push_back(layerId);
    }
    
    /**
     * Remove a layer from this output
     */
    void removeLayer(int layerId) {
        assignedLayers.erase(
            std::remove(assignedLayers.begin(), assignedLayers.end(), layerId),
            assignedLayers.end()
        );
    }
    
    /**
     * Set this output to show all layers
     */
    void showAllLayers() {
        assignedLayers.clear();
    }
    
    /**
     * Check if mode is specified (non-zero)
     */
    bool hasModeOverride() const {
        return width > 0 && height > 0;
    }
};

/**
 * DisplayArrangement - Complete display configuration
 * 
 * A named collection of output configurations representing
 * a complete display setup (e.g., "3-Projector Show", "Single Monitor").
 */
struct DisplayArrangement {
    std::string name = "Default";       // Configuration name
    std::vector<OutputConfig> outputs;  // Per-output configurations
    bool autoDetect = true;             // Auto-configure new outputs
    
    /**
     * Find configuration for an output by name
     * @return Pointer to config, or nullptr if not found
     */
    OutputConfig* findOutput(const std::string& outputName) {
        for (auto& config : outputs) {
            if (config.outputName == outputName) {
                return &config;
            }
        }
        return nullptr;
    }
    
    /**
     * Find configuration for an output by name (const)
     */
    const OutputConfig* findOutput(const std::string& outputName) const {
        for (const auto& config : outputs) {
            if (config.outputName == outputName) {
                return &config;
            }
        }
        return nullptr;
    }
    
    /**
     * Add or update output configuration
     */
    void setOutput(const OutputConfig& config) {
        OutputConfig* existing = findOutput(config.outputName);
        if (existing) {
            *existing = config;
        } else {
            outputs.push_back(config);
        }
    }
    
    /**
     * Remove output configuration by name
     */
    bool removeOutput(const std::string& outputName) {
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (it->outputName == outputName) {
                outputs.erase(it);
                return true;
            }
        }
        return false;
    }
    
    /**
     * Get all outputs that should display a specific layer
     */
    std::vector<std::string> getOutputsForLayer(int layerId) const {
        std::vector<std::string> result;
        for (const auto& config : outputs) {
            if (config.enabled && config.hasLayer(layerId)) {
                result.push_back(config.outputName);
            }
        }
        return result;
    }
    
    /**
     * Get the combined bounding box of all outputs
     */
    void getCombinedBounds(int& outX, int& outY, int& outWidth, int& outHeight) const {
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        bool first = true;
        
        for (const auto& config : outputs) {
            if (!config.enabled) continue;
            
            int right = config.x + config.width;
            int bottom = config.y + config.height;
            
            if (first) {
                minX = config.x;
                minY = config.y;
                maxX = right;
                maxY = bottom;
                first = false;
            } else {
                if (config.x < minX) minX = config.x;
                if (config.y < minY) minY = config.y;
                if (right > maxX) maxX = right;
                if (bottom > maxY) maxY = bottom;
            }
        }
        
        outX = minX;
        outY = minY;
        outWidth = maxX - minX;
        outHeight = maxY - minY;
    }
};

/**
 * VirtualOutputConfig - Configuration for virtual outputs (NDI, streaming)
 * 
 * Used for outputs that don't correspond to physical displays,
 * such as NDI streams, RTSP streams, or file recording.
 */
struct VirtualOutputConfig {
    bool enabled = false;
    std::string sourceName;     // NDI source name or stream URL
    int width = 1920;
    int height = 1080;
    double frameRate = 60.0;
    
    enum class CaptureSource {
        PRIMARY_OUTPUT,      // Capture from primary physical output
        COMPOSITE_FBO,       // Capture from internal composite framebuffer
        SPECIFIC_OUTPUT      // Capture from a specific output by name
    };
    CaptureSource captureSource = CaptureSource::PRIMARY_OUTPUT;
    std::string specificOutputName;  // Used with SPECIFIC_OUTPUT
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTCONFIG_H

