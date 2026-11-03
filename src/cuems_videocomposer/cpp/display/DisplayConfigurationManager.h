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

/**
 * DisplayConfigurationManager.h - Centralized display configuration management
 * 
 * Manages all display-related settings including:
 * - Resolution mode policies
 * - Output region layouts (canvas positions)
 * - Edge blending parameters
 * - Geometric warping
 * - Per-output color correction (color_* keys)
 * - Configuration persistence (save/load)
 */

#ifndef VIDEOCOMPOSER_DISPLAYCONFIGURATIONMANAGER_H
#define VIDEOCOMPOSER_DISPLAYCONFIGURATIONMANAGER_H

#include "OutputRegion.h"
#include "OutputInfo.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>

namespace videocomposer {

/**
 * ResolutionPolicy - How to select display resolution
 */
enum class ResolutionPolicy {
    NATIVE,         // Use EDID preferred mode (panel's true pixels)
    MAXIMUM,        // Use highest available resolution
    HD_1080P,       // Force 1920x1080
    HD_720P,        // Force 1280x720
    UHD_4K,         // Force 3840x2160
    CUSTOM          // Use per-output custom settings
};

/**
 * CanvasLayout - How outputs are arranged on the virtual canvas
 */
enum class CanvasLayout {
    AUTO_HORIZONTAL,  // Side by side, left to right
    AUTO_VERTICAL,    // Stacked, top to bottom
    AUTO_GRID,        // Grid arrangement
    CUSTOM            // Manual positioning via OutputRegion
};

/**
 * Per-output configuration
 */
struct OutputConfiguration {
    std::string name;           // Output name (e.g., "HDMI-A-1")
    
    // Resolution
    int width = 0;              // 0 = use policy default
    int height = 0;
    double refreshRate = 0.0;   // 0 = highest available
    
    // Canvas region
    int canvasX = 0;
    int canvasY = 0;
    int canvasWidth = 0;        // 0 = same as physical
    int canvasHeight = 0;
    
    // Blending
    BlendEdges blend;
    
    // Warping
    bool warpEnabled = false;
    std::string warpMeshPath;   // Path to warp mesh file
    
    // Color (programme phase 3): the color_* keys. Unlike blend, it reaches the
    // output's region under every canvas_layout (applyColorToRegions).
    OutputColor color;
    
    // Status
    bool enabled = true;
    
    /** Any trim off its default, or a LUT configured */
    bool hasColor() const { return color.trimsActive() || color.lutConfigured(); }
};

/**
 * What the last loadFromFile() found wrong with the color_* keys. Every item is
 * also logged when it is found; this keeps a count for diagnostics and the tests.
 */
struct DisplayConfigLoadReport {
    int colorErrors = 0;     // refused values (unparsable, non-finite, refused LUT path)
    int colorWarnings = 0;   // out-of-range values used clamped
    std::vector<std::string> colorWithoutCanvasRegion;  // sections with color_* keys and no canvas_region
};

/**
 * Full display configuration
 */
struct DisplayConfiguration {
    std::string name = "default";
    std::string description;
    
    // Global settings
    ResolutionPolicy resolutionPolicy = ResolutionPolicy::HD_1080P;
    CanvasLayout canvasLayout = CanvasLayout::AUTO_HORIZONTAL;
    
    // Canvas size (0 = auto-calculate from outputs)
    int canvasWidth = 0;
    int canvasHeight = 0;
    
    // Per-output configurations
    std::vector<OutputConfiguration> outputs;
    
    // Metadata
    std::string createdAt;
    std::string modifiedAt;
};

/**
 * DisplayConfigurationManager - Centralized display configuration
 * 
 * Responsibilities:
 * - Store and manage display configurations
 * - Apply resolution policies
 * - Calculate canvas layouts
 * - Persist configurations to disk
 * - Notify listeners of changes
 */
class DisplayConfigurationManager {
public:
    using ConfigChangeCallback = std::function<void(const DisplayConfiguration&)>;
    
    DisplayConfigurationManager();
    ~DisplayConfigurationManager();
    
    // ===== Configuration Management =====
    
    /**
     * Get current configuration
     */
    const DisplayConfiguration& getConfiguration() const { return config_; }
    
    /**
     * Set entire configuration
     */
    void setConfiguration(const DisplayConfiguration& config);
    
    /**
     * Reset to defaults
     */
    void resetToDefaults();
    
    // ===== Resolution Policy =====
    
    /**
     * Set global resolution policy
     */
    void setResolutionPolicy(ResolutionPolicy policy);
    ResolutionPolicy getResolutionPolicy() const { return config_.resolutionPolicy; }
    // True if the most recent loadFromFile() parsed an explicit resolution_policy= line.
    bool wasResolutionPolicySpecified() const { return resolutionPolicySpecified_; }
    
    /**
     * Set resolution policy from string
     * @param policy "native", "maximum", "1080p", "720p", "4k"
     * @return true if valid policy
     */
    bool setResolutionPolicyFromString(const std::string& policy);
    
    /**
     * Get resolution policy as string
     */
    std::string getResolutionPolicyString() const;
    
    /**
     * Get target resolution for a policy
     * @param policy The resolution policy
     * @param outWidth Output width (0 = any)
     * @param outHeight Output height (0 = any)
     * @param preferHighest If true, prefer highest when multiple match
     */
    static void getResolutionForPolicy(ResolutionPolicy policy,
                                       int& outWidth, int& outHeight,
                                       bool& preferHighest);
    
    // ===== Canvas Layout =====
    
    /**
     * Set canvas layout mode
     */
    void setCanvasLayout(CanvasLayout layout);
    CanvasLayout getCanvasLayout() const { return config_.canvasLayout; }
    
    /**
     * Set explicit canvas size (overrides auto-calculation)
     */
    void setCanvasSize(int width, int height);
    
    /**
     * Calculate canvas size from outputs
     */
    void calculateCanvasSize(const std::vector<OutputInfo>& outputs,
                            int& width, int& height) const;
    
    /**
     * Generate output regions based on layout and outputs
     */
    std::vector<OutputRegion> generateOutputRegions(
        const std::vector<OutputInfo>& outputs) const;
    
    // ===== Per-Output Configuration =====
    
    /**
     * Get configuration for an output
     * @param name Output name
     * @return Pointer to config, or nullptr if not found
     */
    OutputConfiguration* getOutputConfig(const std::string& name);
    const OutputConfiguration* getOutputConfig(const std::string& name) const;
    
    /**
     * Set configuration for an output
     */
    void setOutputConfig(const std::string& name, const OutputConfiguration& config);
    
    /**
     * Set output resolution override
     */
    void setOutputResolution(const std::string& name, int width, int height,
                            double refreshRate = 0.0);
    
    /**
     * Set output canvas region
     */
    void setOutputCanvasRegion(const std::string& name,
                               int x, int y, int width, int height);
    
    /**
     * Set output blend parameters
     */
    void setOutputBlend(const std::string& name, const BlendEdges& blend);
    
    /**
     * Copy each configured output's color (trims + LUT path, as parsed) into its
     * region, whatever the canvas layout. Only outputs with color are touched; each
     * gets a fresh draw state, no LUT texture and LutStatus::None - loading the LUT
     * is the backend's (it needs a GL context). GL-free.
     * @return number of regions that received a color
     */
    int applyColorToRegions(std::vector<OutputRegion>& regions) const;
    
    /**
     * Enable/disable output
     */
    void setOutputEnabled(const std::string& name, bool enabled);
    
    // ===== Persistence =====
    
    /**
     * Save configuration to file
     * @param path File path (JSON format)
     * @return true on success
     */
    bool saveToFile(const std::string& path) const;
    
    /**
     * Load configuration from file
     * @param path File path
     * @return true on success
     */
    bool loadFromFile(const std::string& path);
    
    /**
     * What the last loadFromFile() found wrong with the color_* keys
     */
    const DisplayConfigLoadReport& lastLoadReport() const { return loadReport_; }
    
    /**
     * Get default config file path
     */
    static std::string getDefaultConfigPath();
    
    // ===== Change Notifications =====
    
    /**
     * Register callback for configuration changes
     * @return ID for unregistering
     */
    int addChangeListener(ConfigChangeCallback callback);
    
    /**
     * Remove change listener
     */
    void removeChangeListener(int id);
    
private:
    DisplayConfiguration config_;
    std::map<int, ConfigChangeCallback> changeListeners_;
    int nextListenerId_ = 0;
    bool resolutionPolicySpecified_ = false;  // set by loadFromFile() when resolution_policy= is present
    DisplayConfigLoadReport loadReport_;      // filled by loadFromFile()
    
    void notifyListeners();
    void updateTimestamp();
    
    // Ensure output exists in config
    OutputConfiguration& ensureOutputConfig(const std::string& name);
};

// ===== Helper Functions =====

/**
 * Convert ResolutionPolicy to string
 */
inline const char* resolutionPolicyToString(ResolutionPolicy policy) {
    switch (policy) {
        case ResolutionPolicy::NATIVE:    return "native";
        case ResolutionPolicy::MAXIMUM:   return "maximum";
        case ResolutionPolicy::HD_1080P:  return "1080p";
        case ResolutionPolicy::HD_720P:   return "720p";
        case ResolutionPolicy::UHD_4K:    return "4k";
        case ResolutionPolicy::CUSTOM:    return "custom";
        default: return "unknown";
    }
}

/**
 * Convert CanvasLayout to string
 */
inline const char* canvasLayoutToString(CanvasLayout layout) {
    switch (layout) {
        case CanvasLayout::AUTO_HORIZONTAL: return "horizontal";
        case CanvasLayout::AUTO_VERTICAL:   return "vertical";
        case CanvasLayout::AUTO_GRID:       return "grid";
        case CanvasLayout::CUSTOM:          return "custom";
        default: return "unknown";
    }
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DISPLAYCONFIGURATIONMANAGER_H

