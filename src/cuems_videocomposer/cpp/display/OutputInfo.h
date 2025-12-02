/**
 * OutputInfo.h - Common output information structures
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Defines shared data structures for display output metadata that work
 * across all backends (DRM/KMS, X11, Wayland).
 */

#ifndef VIDEOCOMPOSER_OUTPUTINFO_H
#define VIDEOCOMPOSER_OUTPUTINFO_H

#include <string>
#include <vector>
#include <cstdint>

namespace videocomposer {

/**
 * OutputMode - A single display mode (resolution + refresh rate)
 */
struct OutputMode {
    int32_t width = 0;
    int32_t height = 0;
    double refreshRate = 0.0;  // Hz
    bool preferred = false;     // Preferred/native mode
    
    /**
     * Get mode as string (e.g., "1920x1080@60Hz")
     */
    std::string toString() const {
        char buf[64];
        snprintf(buf, sizeof(buf), "%dx%d@%.2fHz", width, height, refreshRate);
        return std::string(buf);
    }
    
    /**
     * Check if modes match (within tolerance for refresh rate)
     */
    bool matches(int w, int h, double refresh = 0.0, double tolerance = 0.5) const {
        if (width != w || height != h) return false;
        if (refresh <= 0.0) return true;  // Don't care about refresh
        return std::abs(refreshRate - refresh) <= tolerance;
    }
};

/**
 * OutputInfo - Full information about a display output
 * 
 * Contains identification, current state, and capabilities of a display.
 * Used across all display backends (DRM, X11, Wayland).
 */
struct OutputInfo {
    // ===== Identification =====
    std::string name;           // Connector name: "HDMI-A-1", "DP-2", "VGA-1", etc.
    std::string make;           // Manufacturer from EDID (e.g., "Samsung", "Dell")
    std::string model;          // Model from EDID (e.g., "U28E590")
    std::string serialNumber;   // Serial number for unique identification
    
    // ===== Current State =====
    int32_t x = 0;              // X position in global coordinate space
    int32_t y = 0;              // Y position in global coordinate space
    int32_t width = 0;          // Current resolution width
    int32_t height = 0;         // Current resolution height
    int32_t physicalWidthMM = 0;   // Physical width in millimeters
    int32_t physicalHeightMM = 0;  // Physical height in millimeters
    double refreshRate = 0.0;   // Current refresh rate in Hz
    int32_t scale = 1;          // HiDPI scale factor (1, 2, etc.)
    
    // ===== Status =====
    bool connected = false;     // Is the output physically connected?
    bool enabled = false;       // Is the output currently active?
    int32_t index = -1;         // Internal index for this output
    
    // ===== Capabilities =====
    std::vector<OutputMode> modes;  // All available display modes
    
    // ===== Helper Methods =====
    
    /**
     * Get DPI (dots per inch) based on physical size
     * @return DPI value, or 96.0 if physical size unknown
     */
    double getDPI() const {
        if (physicalWidthMM <= 0 || width <= 0) {
            return 96.0;  // Default fallback
        }
        // DPI = pixels / inches = pixels / (mm / 25.4)
        return (width * 25.4) / physicalWidthMM;
    }
    
    /**
     * Get the preferred/native mode
     * @return Pointer to preferred mode, or nullptr if none found
     */
    const OutputMode* getPreferredMode() const {
        for (const auto& mode : modes) {
            if (mode.preferred) {
                return &mode;
            }
        }
        // Fallback: return highest resolution mode
        const OutputMode* best = nullptr;
        for (const auto& mode : modes) {
            if (!best || (mode.width * mode.height) > (best->width * best->height)) {
                best = &mode;
            }
        }
        return best;
    }
    
    /**
     * Find a specific mode by resolution and optional refresh rate
     * @param w Width
     * @param h Height
     * @param refresh Refresh rate (0 = don't care)
     * @return Pointer to matching mode, or nullptr if not found
     */
    const OutputMode* findMode(int w, int h, double refresh = 0.0) const {
        for (const auto& mode : modes) {
            if (mode.matches(w, h, refresh)) {
                return &mode;
            }
        }
        return nullptr;
    }
    
    /**
     * Get display name for UI (e.g., "Samsung U28E590 (HDMI-A-1)")
     */
    std::string getDisplayName() const {
        std::string result;
        if (!make.empty()) {
            result = make;
        }
        if (!model.empty()) {
            if (!result.empty()) result += " ";
            result += model;
        }
        if (!name.empty()) {
            if (!result.empty()) {
                result += " (" + name + ")";
            } else {
                result = name;
            }
        }
        return result.empty() ? "Unknown Display" : result;
    }
    
    /**
     * Get current mode as OutputMode
     */
    OutputMode getCurrentMode() const {
        OutputMode mode;
        mode.width = width;
        mode.height = height;
        mode.refreshRate = refreshRate;
        mode.preferred = false;
        return mode;
    }
    
    /**
     * Check if this output contains a point in global coordinates
     */
    bool containsPoint(int px, int py) const {
        return px >= x && px < (x + width) && py >= y && py < (y + height);
    }
    
    /**
     * Get output bounds as a rect
     */
    void getBounds(int& outX, int& outY, int& outWidth, int& outHeight) const {
        outX = x;
        outY = y;
        outWidth = width;
        outHeight = height;
    }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTINFO_H

