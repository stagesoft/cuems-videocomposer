/**
 * DisplayConfigurationManager.cpp - Display configuration implementation
 */

#include "DisplayConfigurationManager.h"
#include "../utils/Logger.h"

#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <cctype>

namespace videocomposer {

DisplayConfigurationManager::DisplayConfigurationManager() {
    resetToDefaults();
}

DisplayConfigurationManager::~DisplayConfigurationManager() {
}

void DisplayConfigurationManager::setConfiguration(const DisplayConfiguration& config) {
    config_ = config;
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::resetToDefaults() {
    config_ = DisplayConfiguration();
    config_.name = "default";
    config_.resolutionPolicy = ResolutionPolicy::HD_1080P;
    config_.canvasLayout = CanvasLayout::AUTO_HORIZONTAL;
    config_.outputs.clear();
    updateTimestamp();
}

// ============================================================================
// Resolution Policy
// ============================================================================

void DisplayConfigurationManager::setResolutionPolicy(ResolutionPolicy policy) {
    if (config_.resolutionPolicy != policy) {
        config_.resolutionPolicy = policy;
        LOG_INFO << "DisplayConfigurationManager: Resolution policy set to "
                 << resolutionPolicyToString(policy);
        updateTimestamp();
        notifyListeners();
    }
}

bool DisplayConfigurationManager::setResolutionPolicyFromString(const std::string& policy) {
    std::string p = policy;
    for (auto& c : p) {
        c = std::tolower(c);
    }
    
    if (p == "native") {
        setResolutionPolicy(ResolutionPolicy::NATIVE);
    } else if (p == "maximum" || p == "max" || p == "highest") {
        setResolutionPolicy(ResolutionPolicy::MAXIMUM);
    } else if (p == "1080p" || p == "1080" || p == "fhd") {
        setResolutionPolicy(ResolutionPolicy::HD_1080P);
    } else if (p == "720p" || p == "720" || p == "hd") {
        setResolutionPolicy(ResolutionPolicy::HD_720P);
    } else if (p == "4k" || p == "uhd" || p == "2160p") {
        setResolutionPolicy(ResolutionPolicy::UHD_4K);
    } else if (p == "custom") {
        setResolutionPolicy(ResolutionPolicy::CUSTOM);
    } else {
        LOG_WARNING << "DisplayConfigurationManager: Unknown resolution policy: " << policy;
        return false;
    }
    
    return true;
}

std::string DisplayConfigurationManager::getResolutionPolicyString() const {
    return resolutionPolicyToString(config_.resolutionPolicy);
}

void DisplayConfigurationManager::getResolutionForPolicy(ResolutionPolicy policy,
                                                          int& outWidth, int& outHeight,
                                                          bool& preferHighest) {
    outWidth = 0;
    outHeight = 0;
    preferHighest = false;
    
    switch (policy) {
        case ResolutionPolicy::NATIVE:
            // Use EDID preferred - let the backend find it
            preferHighest = false;
            break;
            
        case ResolutionPolicy::MAXIMUM:
            // Use highest available
            preferHighest = true;
            break;
            
        case ResolutionPolicy::HD_1080P:
            outWidth = 1920;
            outHeight = 1080;
            break;
            
        case ResolutionPolicy::HD_720P:
            outWidth = 1280;
            outHeight = 720;
            break;
            
        case ResolutionPolicy::UHD_4K:
            outWidth = 3840;
            outHeight = 2160;
            break;
            
        case ResolutionPolicy::CUSTOM:
            // Per-output settings
            break;
    }
}

// ============================================================================
// Canvas Layout
// ============================================================================

void DisplayConfigurationManager::setCanvasLayout(CanvasLayout layout) {
    if (config_.canvasLayout != layout) {
        config_.canvasLayout = layout;
        LOG_INFO << "DisplayConfigurationManager: Canvas layout set to "
                 << canvasLayoutToString(layout);
        updateTimestamp();
        notifyListeners();
    }
}

void DisplayConfigurationManager::setCanvasSize(int width, int height) {
    config_.canvasWidth = width;
    config_.canvasHeight = height;
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::calculateCanvasSize(const std::vector<OutputInfo>& outputs,
                                                       int& width, int& height) const {
    width = 0;
    height = 0;
    
    if (config_.canvasWidth > 0 && config_.canvasHeight > 0) {
        // Use explicit size
        width = config_.canvasWidth;
        height = config_.canvasHeight;
        return;
    }
    
    // Calculate from outputs based on layout
    switch (config_.canvasLayout) {
        case CanvasLayout::AUTO_HORIZONTAL:
            for (const auto& out : outputs) {
                if (out.enabled) {
                    width += out.width;
                    height = std::max(height, out.height);
                }
            }
            break;
            
        case CanvasLayout::AUTO_VERTICAL:
            for (const auto& out : outputs) {
                if (out.enabled) {
                    width = std::max(width, out.width);
                    height += out.height;
                }
            }
            break;
            
        case CanvasLayout::AUTO_GRID: {
            // Simple 2-column grid
            int cols = 2;
            int rows = (static_cast<int>(outputs.size()) + cols - 1) / cols;
            int maxW = 0, maxH = 0;
            for (const auto& out : outputs) {
                if (out.enabled) {
                    maxW = std::max(maxW, out.width);
                    maxH = std::max(maxH, out.height);
                }
            }
            width = maxW * cols;
            height = maxH * rows;
            break;
        }
            
        case CanvasLayout::CUSTOM:
            // Use output configurations
            for (const auto& outConfig : config_.outputs) {
                int right = outConfig.canvasX + 
                    (outConfig.canvasWidth > 0 ? outConfig.canvasWidth : outConfig.width);
                int bottom = outConfig.canvasY + 
                    (outConfig.canvasHeight > 0 ? outConfig.canvasHeight : outConfig.height);
                width = std::max(width, right);
                height = std::max(height, bottom);
            }
            break;
    }
}

std::vector<OutputRegion> DisplayConfigurationManager::generateOutputRegions(
    const std::vector<OutputInfo>& outputs) const {
    
    std::vector<OutputRegion> regions;
    int x = 0, y = 0;
    int col = 0;
    int rowHeight = 0;
    
    for (size_t i = 0; i < outputs.size(); ++i) {
        const auto& out = outputs[i];
        if (!out.enabled) continue;
        
        OutputRegion region = OutputRegion::createDefault(
            out.name,
            out.width, out.height, 0, 0
        );
        
        // Check for custom configuration
        const OutputConfiguration* customConfig = getOutputConfig(out.name);
        if (customConfig && config_.canvasLayout == CanvasLayout::CUSTOM) {
            region.canvasX = customConfig->canvasX;
            region.canvasY = customConfig->canvasY;
            region.canvasWidth = customConfig->canvasWidth > 0 ? 
                                 customConfig->canvasWidth : out.width;
            region.canvasHeight = customConfig->canvasHeight > 0 ? 
                                  customConfig->canvasHeight : out.height;
            region.blend = customConfig->blend;
        } else {
            // Auto layout
            switch (config_.canvasLayout) {
                case CanvasLayout::AUTO_HORIZONTAL:
                    region.canvasX = x;
                    region.canvasY = 0;
                    region.canvasWidth = out.width;
                    region.canvasHeight = out.height;
                    x += out.width;
                    break;
                    
                case CanvasLayout::AUTO_VERTICAL:
                    region.canvasX = 0;
                    region.canvasY = y;
                    region.canvasWidth = out.width;
                    region.canvasHeight = out.height;
                    y += out.height;
                    break;
                    
                case CanvasLayout::AUTO_GRID:
                    region.canvasX = x;
                    region.canvasY = y;
                    region.canvasWidth = out.width;
                    region.canvasHeight = out.height;
                    rowHeight = std::max(rowHeight, out.height);
                    col++;
                    if (col >= 2) {
                        col = 0;
                        x = 0;
                        y += rowHeight;
                        rowHeight = 0;
                    } else {
                        x += out.width;
                    }
                    break;
                    
                default:
                    break;
            }
        }
        
        regions.push_back(region);
    }
    
    return regions;
}

// ============================================================================
// Per-Output Configuration
// ============================================================================

OutputConfiguration* DisplayConfigurationManager::getOutputConfig(const std::string& name) {
    for (auto& out : config_.outputs) {
        if (out.name == name) {
            return &out;
        }
    }
    return nullptr;
}

const OutputConfiguration* DisplayConfigurationManager::getOutputConfig(const std::string& name) const {
    for (const auto& out : config_.outputs) {
        if (out.name == name) {
            return &out;
        }
    }
    return nullptr;
}

OutputConfiguration& DisplayConfigurationManager::ensureOutputConfig(const std::string& name) {
    for (auto& out : config_.outputs) {
        if (out.name == name) {
            return out;
        }
    }
    
    // Create new
    OutputConfiguration newConfig;
    newConfig.name = name;
    config_.outputs.push_back(newConfig);
    return config_.outputs.back();
}

void DisplayConfigurationManager::setOutputConfig(const std::string& name,
                                                   const OutputConfiguration& config) {
    OutputConfiguration& existing = ensureOutputConfig(name);
    existing = config;
    existing.name = name;  // Ensure name is preserved
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::setOutputResolution(const std::string& name,
                                                       int width, int height,
                                                       double refreshRate) {
    OutputConfiguration& out = ensureOutputConfig(name);
    out.width = width;
    out.height = height;
    out.refreshRate = refreshRate;
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::setOutputCanvasRegion(const std::string& name,
                                                         int x, int y,
                                                         int width, int height) {
    OutputConfiguration& out = ensureOutputConfig(name);
    out.canvasX = x;
    out.canvasY = y;
    out.canvasWidth = width;
    out.canvasHeight = height;
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::setOutputBlend(const std::string& name,
                                                  const BlendEdges& blend) {
    OutputConfiguration& out = ensureOutputConfig(name);
    out.blend = blend;
    updateTimestamp();
    notifyListeners();
}

void DisplayConfigurationManager::setOutputEnabled(const std::string& name, bool enabled) {
    OutputConfiguration& out = ensureOutputConfig(name);
    out.enabled = enabled;
    updateTimestamp();
    notifyListeners();
}

// ============================================================================
// Persistence
// ============================================================================

bool DisplayConfigurationManager::saveToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR << "DisplayConfigurationManager: Failed to open file for writing: " << path;
        return false;
    }
    
    // Simple JSON-like format (proper JSON would need a library)
    file << "# Display Configuration\n";
    file << "# Generated by cuems-videocomposer\n\n";
    
    file << "name=" << config_.name << "\n";
    file << "resolution_policy=" << resolutionPolicyToString(config_.resolutionPolicy) << "\n";
    file << "canvas_layout=" << canvasLayoutToString(config_.canvasLayout) << "\n";
    
    if (config_.canvasWidth > 0 && config_.canvasHeight > 0) {
        file << "canvas_size=" << config_.canvasWidth << "x" << config_.canvasHeight << "\n";
    }
    
    file << "\n# Per-output configurations\n";
    for (const auto& out : config_.outputs) {
        file << "[output:" << out.name << "]\n";
        if (out.width > 0 && out.height > 0) {
            file << "resolution=" << out.width << "x" << out.height << "\n";
        }
        if (out.refreshRate > 0) {
            file << "refresh=" << out.refreshRate << "\n";
        }
        file << "canvas_region=" << out.canvasX << "," << out.canvasY 
             << "," << out.canvasWidth << "," << out.canvasHeight << "\n";
        file << "blend=" << out.blend.left << "," << out.blend.right
             << "," << out.blend.top << "," << out.blend.bottom
             << "," << out.blend.gamma << "\n";
        file << "enabled=" << (out.enabled ? "true" : "false") << "\n";
        file << "\n";
    }
    
    LOG_INFO << "DisplayConfigurationManager: Saved configuration to " << path;
    return true;
}

bool DisplayConfigurationManager::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARNING << "DisplayConfigurationManager: Failed to open file: " << path;
        return false;
    }
    
    resetToDefaults();
    
    std::string line;
    std::string currentOutput;
    
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;
        
        // Check for output section
        if (line.find("[output:") == 0) {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                currentOutput = line.substr(8, end - 8);
                ensureOutputConfig(currentOutput);
            }
            continue;
        }
        
        // Parse key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        
        if (currentOutput.empty()) {
            // Global settings
            if (key == "name") {
                config_.name = value;
            } else if (key == "resolution_policy") {
                setResolutionPolicyFromString(value);
            } else if (key == "canvas_layout") {
                if (value == "horizontal") setCanvasLayout(CanvasLayout::AUTO_HORIZONTAL);
                else if (value == "vertical") setCanvasLayout(CanvasLayout::AUTO_VERTICAL);
                else if (value == "grid") setCanvasLayout(CanvasLayout::AUTO_GRID);
                else if (value == "custom") setCanvasLayout(CanvasLayout::CUSTOM);
            } else if (key == "canvas_size") {
                int w, h;
                if (sscanf(value.c_str(), "%dx%d", &w, &h) == 2) {
                    setCanvasSize(w, h);
                }
            }
        } else {
            // Per-output settings
            OutputConfiguration* out = getOutputConfig(currentOutput);
            if (!out) continue;
            
            if (key == "resolution") {
                sscanf(value.c_str(), "%dx%d", &out->width, &out->height);
            } else if (key == "refresh") {
                out->refreshRate = std::stod(value);
            } else if (key == "canvas_region") {
                sscanf(value.c_str(), "%d,%d,%d,%d",
                       &out->canvasX, &out->canvasY,
                       &out->canvasWidth, &out->canvasHeight);
            } else if (key == "blend") {
                sscanf(value.c_str(), "%f,%f,%f,%f,%f",
                       &out->blend.left, &out->blend.right,
                       &out->blend.top, &out->blend.bottom,
                       &out->blend.gamma);
            } else if (key == "enabled") {
                out->enabled = (value == "true" || value == "1");
            }
        }
    }
    
    LOG_INFO << "DisplayConfigurationManager: Loaded configuration from " << path;
    notifyListeners();
    return true;
}

std::string DisplayConfigurationManager::getDefaultConfigPath() {
    // Use XDG config directory if available
    const char* xdgConfig = getenv("XDG_CONFIG_HOME");
    if (xdgConfig) {
        return std::string(xdgConfig) + "/cuems-videocomposer/display.conf";
    }
    
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/.config/cuems-videocomposer/display.conf";
    }
    
    return "display.conf";
}

// ============================================================================
// Change Notifications
// ============================================================================

int DisplayConfigurationManager::addChangeListener(ConfigChangeCallback callback) {
    int id = nextListenerId_++;
    changeListeners_[id] = callback;
    return id;
}

void DisplayConfigurationManager::removeChangeListener(int id) {
    changeListeners_.erase(id);
}

void DisplayConfigurationManager::notifyListeners() {
    for (const auto& pair : changeListeners_) {
        pair.second(config_);
    }
}

void DisplayConfigurationManager::updateTimestamp() {
    time_t now = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    config_.modifiedAt = buf;
    
    if (config_.createdAt.empty()) {
        config_.createdAt = config_.modifiedAt;
    }
}

} // namespace videocomposer

