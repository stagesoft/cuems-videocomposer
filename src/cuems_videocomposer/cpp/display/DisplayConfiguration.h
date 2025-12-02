/**
 * DisplayConfiguration.h - Display configuration management
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Provides configuration management for multi-display setups including:
 * - Load/save JSON configuration files
 * - Per-output configuration management
 * - Layer routing
 * - Auto-configuration for new outputs
 */

#ifndef VIDEOCOMPOSER_DISPLAYCONFIGURATION_H
#define VIDEOCOMPOSER_DISPLAYCONFIGURATION_H

#include "OutputInfo.h"
#include "OutputConfig.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace videocomposer {

/**
 * DisplayConfiguration - Main configuration manager for displays
 * 
 * Manages display arrangements, per-output configuration, and layer routing.
 * Supports loading/saving JSON configuration files.
 */
class DisplayConfiguration {
public:
    DisplayConfiguration();
    ~DisplayConfiguration();
    
    // ===== Load/Save =====
    
    /**
     * Load configuration from a JSON file
     * @param path Path to JSON configuration file
     * @return true on success, false on failure
     */
    bool loadFromFile(const std::string& path);
    
    /**
     * Save configuration to a JSON file
     * @param path Path to save JSON configuration
     * @return true on success, false on failure
     */
    bool saveToFile(const std::string& path) const;
    
    /**
     * Load configuration from a JSON string
     * @param json JSON string to parse
     * @return true on success, false on failure
     */
    bool loadFromString(const std::string& json);
    
    /**
     * Serialize configuration to a JSON string
     * @return JSON string representation
     */
    std::string saveToString() const;
    
    // ===== Arrangement Management =====
    
    /**
     * Set the current display arrangement
     */
    void setArrangement(const DisplayArrangement& arrangement);
    
    /**
     * Get the current display arrangement
     */
    const DisplayArrangement& getArrangement() const { return arrangement_; }
    
    /**
     * Get the arrangement name
     */
    const std::string& getArrangementName() const { return arrangement_.name; }
    
    /**
     * Set the arrangement name
     */
    void setArrangementName(const std::string& name) { arrangement_.name = name; }
    
    // ===== Per-Output Configuration =====
    
    /**
     * Get configuration for a specific output
     * @param outputName Output connector name (e.g., "HDMI-A-1")
     * @return Pointer to config, or nullptr if not found
     */
    OutputConfig* getOutputConfig(const std::string& outputName);
    
    /**
     * Get configuration for a specific output (const)
     */
    const OutputConfig* getOutputConfig(const std::string& outputName) const;
    
    /**
     * Set configuration for an output
     * @param outputName Output connector name
     * @param config Configuration to set
     */
    void setOutputConfig(const std::string& outputName, const OutputConfig& config);
    
    /**
     * Remove configuration for an output
     */
    bool removeOutputConfig(const std::string& outputName);
    
    /**
     * Get all configured output names
     */
    std::vector<std::string> getConfiguredOutputs() const;
    
    // ===== Layer Routing =====
    
    /**
     * Get all outputs that should display a specific layer
     */
    std::vector<std::string> getOutputsForLayer(int layerId) const;
    
    /**
     * Assign a layer to a specific output
     */
    void assignLayerToOutput(int layerId, const std::string& outputName);
    
    /**
     * Assign a layer to all outputs
     */
    void assignLayerToAllOutputs(int layerId);
    
    /**
     * Remove a layer from an output
     */
    void removeLayerFromOutput(int layerId, const std::string& outputName);
    
    // ===== Configuration Application =====
    
    /**
     * Apply configuration using a callback for each output
     */
    using ConfigApplier = std::function<bool(const std::string&, const OutputConfig&)>;
    bool applyToOutputs(ConfigApplier applier) const;
    
    // ===== Auto-Configuration =====
    
    /**
     * Create a default configuration for a new output
     */
    OutputConfig createDefaultConfig(const OutputInfo& info) const;
    
    /**
     * Auto-configure a new output (add to arrangement if autoDetect is enabled)
     */
    void autoConfigureNewOutput(const OutputInfo& info);
    
    /**
     * Check if auto-detection is enabled
     */
    bool isAutoDetectEnabled() const { return arrangement_.autoDetect; }
    
    /**
     * Enable/disable auto-detection
     */
    void setAutoDetect(bool enabled) { arrangement_.autoDetect = enabled; }
    
    // ===== Virtual Output Configuration =====
    
    /**
     * Get virtual output configuration (NDI, streaming)
     */
    const VirtualOutputConfig& getVirtualOutputConfig() const { return virtualOutput_; }
    
    /**
     * Set virtual output configuration
     */
    void setVirtualOutputConfig(const VirtualOutputConfig& config) { virtualOutput_ = config; }
    
    /**
     * Check if running in headless mode (no physical displays)
     */
    bool isHeadless() const { return headless_; }
    
    /**
     * Set headless mode
     */
    void setHeadless(bool headless) { headless_ = headless; }
    
private:
    DisplayArrangement arrangement_;
    std::map<std::string, OutputConfig> outputConfigs_;
    VirtualOutputConfig virtualOutput_;
    std::string configPath_;
    bool headless_ = false;
    
    // JSON parsing helpers (simple implementation without external library)
    bool parseJson(const std::string& json);
    std::string generateJson() const;
    
    // Helper to trim whitespace
    static std::string trim(const std::string& str);
    
    // Helper to extract string value from JSON
    static bool extractString(const std::string& json, const std::string& key, std::string& value);
    static bool extractInt(const std::string& json, const std::string& key, int32_t& value);
    static bool extractDouble(const std::string& json, const std::string& key, double& value);
    static bool extractBool(const std::string& json, const std::string& key, bool& value);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DISPLAYCONFIGURATION_H

