/**
 * DisplayConfiguration.cpp - Display configuration implementation
 * 
 * Implements JSON loading/saving and configuration management for
 * multi-display setups.
 */

#include "DisplayConfiguration.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <cmath>

namespace videocomposer {

DisplayConfiguration::DisplayConfiguration() {
    arrangement_.name = "Default";
    arrangement_.autoDetect = true;
}

DisplayConfiguration::~DisplayConfiguration() {
}

// ===== Load/Save =====

bool DisplayConfiguration::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARNING << "Could not open display config file: " << path;
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();
    file.close();
    
    if (!parseJson(json)) {
        LOG_ERROR << "Failed to parse display config: " << path;
        return false;
    }
    
    configPath_ = path;
    LOG_INFO << "Loaded display configuration: " << arrangement_.name 
             << " (" << arrangement_.outputs.size() << " outputs)";
    return true;
}

bool DisplayConfiguration::saveToFile(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR << "Could not open file for writing: " << path;
        return false;
    }
    
    file << generateJson();
    file.close();
    
    LOG_INFO << "Saved display configuration to: " << path;
    return true;
}

bool DisplayConfiguration::loadFromString(const std::string& json) {
    return parseJson(json);
}

std::string DisplayConfiguration::saveToString() const {
    return generateJson();
}

// ===== Arrangement Management =====

void DisplayConfiguration::setArrangement(const DisplayArrangement& arrangement) {
    arrangement_ = arrangement;
    
    // Rebuild outputConfigs_ map
    outputConfigs_.clear();
    for (const auto& config : arrangement_.outputs) {
        outputConfigs_[config.outputName] = config;
    }
}

// ===== Per-Output Configuration =====

OutputConfig* DisplayConfiguration::getOutputConfig(const std::string& outputName) {
    auto it = outputConfigs_.find(outputName);
    if (it != outputConfigs_.end()) {
        return &it->second;
    }
    return nullptr;
}

const OutputConfig* DisplayConfiguration::getOutputConfig(const std::string& outputName) const {
    auto it = outputConfigs_.find(outputName);
    if (it != outputConfigs_.end()) {
        return &it->second;
    }
    return nullptr;
}

void DisplayConfiguration::setOutputConfig(const std::string& outputName, const OutputConfig& config) {
    outputConfigs_[outputName] = config;
    
    // Update arrangement
    OutputConfig* existing = arrangement_.findOutput(outputName);
    if (existing) {
        *existing = config;
    } else {
        arrangement_.outputs.push_back(config);
    }
}

bool DisplayConfiguration::removeOutputConfig(const std::string& outputName) {
    auto it = outputConfigs_.find(outputName);
    if (it != outputConfigs_.end()) {
        outputConfigs_.erase(it);
        arrangement_.removeOutput(outputName);
        return true;
    }
    return false;
}

std::vector<std::string> DisplayConfiguration::getConfiguredOutputs() const {
    std::vector<std::string> result;
    for (const auto& pair : outputConfigs_) {
        result.push_back(pair.first);
    }
    return result;
}

// ===== Layer Routing =====

std::vector<std::string> DisplayConfiguration::getOutputsForLayer(int layerId) const {
    return arrangement_.getOutputsForLayer(layerId);
}

void DisplayConfiguration::assignLayerToOutput(int layerId, const std::string& outputName) {
    OutputConfig* config = getOutputConfig(outputName);
    if (config) {
        config->addLayer(layerId);
    }
}

void DisplayConfiguration::assignLayerToAllOutputs(int layerId) {
    for (auto& pair : outputConfigs_) {
        pair.second.showAllLayers();
    }
}

void DisplayConfiguration::removeLayerFromOutput(int layerId, const std::string& outputName) {
    OutputConfig* config = getOutputConfig(outputName);
    if (config) {
        config->removeLayer(layerId);
    }
}

// ===== Configuration Application =====

bool DisplayConfiguration::applyToOutputs(ConfigApplier applier) const {
    bool success = true;
    for (const auto& config : arrangement_.outputs) {
        if (config.enabled) {
            if (!applier(config.outputName, config)) {
                LOG_ERROR << "Failed to apply config for output: " << config.outputName;
                success = false;
            }
        }
    }
    return success;
}

// ===== Auto-Configuration =====

OutputConfig DisplayConfiguration::createDefaultConfig(const OutputInfo& info) const {
    OutputConfig config;
    config.outputName = info.name;
    config.x = info.x;
    config.y = info.y;
    config.width = info.width;
    config.height = info.height;
    config.refreshRate = info.refreshRate;
    config.enabled = true;
    config.rotation = 0;
    // Default: show all layers
    config.assignedLayers.clear();
    config.blendEnabled = false;
    config.warpEnabled = false;
    return config;
}

void DisplayConfiguration::autoConfigureNewOutput(const OutputInfo& info) {
    if (!arrangement_.autoDetect) {
        return;
    }
    
    // Check if already configured
    if (getOutputConfig(info.name) != nullptr) {
        return;
    }
    
    OutputConfig config = createDefaultConfig(info);
    setOutputConfig(info.name, config);
    LOG_INFO << "Auto-configured new output: " << info.name 
             << " (" << info.width << "x" << info.height << "@" 
             << info.refreshRate << "Hz)";
}

// ===== JSON Helpers =====

std::string DisplayConfiguration::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

bool DisplayConfiguration::extractString(const std::string& json, const std::string& key, std::string& value) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1) {
        value = match[1].str();
        return true;
    }
    return false;
}

bool DisplayConfiguration::extractInt(const std::string& json, const std::string& key, int32_t& value) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(-?\\d+)";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1) {
        value = std::stoi(match[1].str());
        return true;
    }
    return false;
}

bool DisplayConfiguration::extractDouble(const std::string& json, const std::string& key, double& value) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(-?\\d+\\.?\\d*)";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1) {
        value = std::stod(match[1].str());
        return true;
    }
    return false;
}

bool DisplayConfiguration::extractBool(const std::string& json, const std::string& key, bool& value) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(true|false)";
    std::regex re(pattern, std::regex::icase);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1) {
        std::string val = match[1].str();
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        value = (val == "true");
        return true;
    }
    return false;
}

bool DisplayConfiguration::parseJson(const std::string& json) {
    try {
        // Extract top-level fields
        extractString(json, "name", arrangement_.name);
        extractBool(json, "autoDetect", arrangement_.autoDetect);
        extractBool(json, "headless", headless_);
        
        // Find outputs array
        std::regex outputsRe("\"outputs\"\\s*:\\s*\\[([^\\]]*)\\]", std::regex::extended);
        std::smatch outputsMatch;
        if (std::regex_search(json, outputsMatch, outputsRe) && outputsMatch.size() > 1) {
            std::string outputsJson = outputsMatch[1].str();
            
            // Split into individual output objects
            std::regex objRe("\\{([^{}]*)\\}");
            std::sregex_iterator iter(outputsJson.begin(), outputsJson.end(), objRe);
            std::sregex_iterator end;
            
            arrangement_.outputs.clear();
            outputConfigs_.clear();
            
            while (iter != end) {
                std::string outputJson = iter->str();
                OutputConfig config;
                
                // Extract output fields
                extractString(outputJson, "name", config.outputName);
                extractInt(outputJson, "x", config.x);
                extractInt(outputJson, "y", config.y);
                extractInt(outputJson, "width", config.width);
                extractInt(outputJson, "height", config.height);
                extractDouble(outputJson, "refresh", config.refreshRate);
                extractBool(outputJson, "enabled", config.enabled);
                extractInt(outputJson, "rotation", config.rotation);
                
                // Extract layers array
                std::regex layersRe("\"layers\"\\s*:\\s*\\[([^\\]]*)\\]");
                std::smatch layersMatch;
                if (std::regex_search(outputJson, layersMatch, layersRe) && layersMatch.size() > 1) {
                    std::string layersStr = layersMatch[1].str();
                    std::regex numRe("-?\\d+");
                    std::sregex_iterator numIter(layersStr.begin(), layersStr.end(), numRe);
                    std::sregex_iterator numEnd;
                    while (numIter != numEnd) {
                        config.assignedLayers.push_back(std::stoi(numIter->str()));
                        ++numIter;
                    }
                }
                
                // Extract blend settings
                std::regex blendRe("\"blend\"\\s*:\\s*\\{([^{}]*)\\}");
                std::smatch blendMatch;
                if (std::regex_search(outputJson, blendMatch, blendRe) && blendMatch.size() > 1) {
                    std::string blendJson = blendMatch[1].str();
                    config.blendEnabled = true;
                    
                    double val;
                    if (extractDouble("{" + blendJson + "}", "left", val)) config.blend.left = static_cast<float>(val);
                    if (extractDouble("{" + blendJson + "}", "right", val)) config.blend.right = static_cast<float>(val);
                    if (extractDouble("{" + blendJson + "}", "top", val)) config.blend.top = static_cast<float>(val);
                    if (extractDouble("{" + blendJson + "}", "bottom", val)) config.blend.bottom = static_cast<float>(val);
                    if (extractDouble("{" + blendJson + "}", "gamma", val)) config.blend.gamma = static_cast<float>(val);
                }
                
                // Extract warp settings
                std::regex warpRe("\"warp\"\\s*:\\s*\\{([^{}]*)\\}");
                std::smatch warpMatch;
                if (std::regex_search(outputJson, warpMatch, warpRe) && warpMatch.size() > 1) {
                    std::string warpJson = warpMatch[1].str();
                    extractBool("{" + warpJson + "}", "enabled", config.warpEnabled);
                    extractString("{" + warpJson + "}", "meshPath", config.warpMeshPath);
                }
                
                // Extract NDI capture flag
                extractBool(outputJson, "captureForNDI", config.captureForNDI);
                
                arrangement_.outputs.push_back(config);
                outputConfigs_[config.outputName] = config;
                
                ++iter;
            }
        }
        
        // Parse virtual output config
        std::regex virtualRe("\"virtualOutputs\"\\s*:\\s*\\{([^{}]*)\\}");
        std::smatch virtualMatch;
        if (std::regex_search(json, virtualMatch, virtualRe) && virtualMatch.size() > 1) {
            std::string virtualJson = "{" + virtualMatch[1].str() + "}";
            
            // Look for NDI sub-object
            std::regex ndiRe("\"ndi\"\\s*:\\s*\\{([^{}]*)\\}");
            std::smatch ndiMatch;
            if (std::regex_search(virtualJson, ndiMatch, ndiRe) && ndiMatch.size() > 1) {
                std::string ndiJson = "{" + ndiMatch[1].str() + "}";
                extractBool(ndiJson, "enabled", virtualOutput_.enabled);
                extractString(ndiJson, "sourceName", virtualOutput_.sourceName);
                extractInt(ndiJson, "width", virtualOutput_.width);
                extractInt(ndiJson, "height", virtualOutput_.height);
                extractDouble(ndiJson, "frameRate", virtualOutput_.frameRate);
                
                std::string captureSourceStr;
                if (extractString(ndiJson, "captureSource", captureSourceStr)) {
                    if (captureSourceStr == "PRIMARY_OUTPUT") {
                        virtualOutput_.captureSource = VirtualOutputConfig::CaptureSource::PRIMARY_OUTPUT;
                    } else if (captureSourceStr == "COMPOSITE_FBO") {
                        virtualOutput_.captureSource = VirtualOutputConfig::CaptureSource::COMPOSITE_FBO;
                    } else if (captureSourceStr == "SPECIFIC_OUTPUT") {
                        virtualOutput_.captureSource = VirtualOutputConfig::CaptureSource::SPECIFIC_OUTPUT;
                    }
                }
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "JSON parsing error: " << e.what();
        return false;
    }
}

std::string DisplayConfiguration::generateJson() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "    \"name\": \"" << arrangement_.name << "\",\n";
    ss << "    \"autoDetect\": " << (arrangement_.autoDetect ? "true" : "false") << ",\n";
    ss << "    \"headless\": " << (headless_ ? "true" : "false") << ",\n";
    
    // Outputs array
    ss << "    \"outputs\": [\n";
    for (size_t i = 0; i < arrangement_.outputs.size(); ++i) {
        const auto& config = arrangement_.outputs[i];
        ss << "        {\n";
        ss << "            \"name\": \"" << config.outputName << "\",\n";
        ss << "            \"x\": " << config.x << ",\n";
        ss << "            \"y\": " << config.y << ",\n";
        ss << "            \"width\": " << config.width << ",\n";
        ss << "            \"height\": " << config.height << ",\n";
        ss << "            \"refresh\": " << config.refreshRate << ",\n";
        ss << "            \"enabled\": " << (config.enabled ? "true" : "false") << ",\n";
        ss << "            \"rotation\": " << config.rotation << ",\n";
        
        // Layers
        ss << "            \"layers\": [";
        for (size_t j = 0; j < config.assignedLayers.size(); ++j) {
            if (j > 0) ss << ", ";
            ss << config.assignedLayers[j];
        }
        ss << "]";
        
        // Blend
        if (config.blendEnabled || config.blend.isEnabled()) {
            ss << ",\n            \"blend\": {\n";
            ss << "                \"left\": " << config.blend.left << ",\n";
            ss << "                \"right\": " << config.blend.right << ",\n";
            ss << "                \"top\": " << config.blend.top << ",\n";
            ss << "                \"bottom\": " << config.blend.bottom << ",\n";
            ss << "                \"gamma\": " << config.blend.gamma << "\n";
            ss << "            }";
        }
        
        // Warp
        if (config.warpEnabled) {
            ss << ",\n            \"warp\": {\n";
            ss << "                \"enabled\": true,\n";
            ss << "                \"meshPath\": \"" << config.warpMeshPath << "\"\n";
            ss << "            }";
        }
        
        // NDI capture
        if (config.captureForNDI) {
            ss << ",\n            \"captureForNDI\": true";
        }
        
        ss << "\n        }";
        if (i < arrangement_.outputs.size() - 1) ss << ",";
        ss << "\n";
    }
    ss << "    ]";
    
    // Virtual outputs
    if (virtualOutput_.enabled) {
        ss << ",\n    \"virtualOutputs\": {\n";
        ss << "        \"ndi\": {\n";
        ss << "            \"enabled\": true,\n";
        ss << "            \"sourceName\": \"" << virtualOutput_.sourceName << "\",\n";
        ss << "            \"width\": " << virtualOutput_.width << ",\n";
        ss << "            \"height\": " << virtualOutput_.height << ",\n";
        ss << "            \"frameRate\": " << virtualOutput_.frameRate << ",\n";
        ss << "            \"captureSource\": \"";
        switch (virtualOutput_.captureSource) {
            case VirtualOutputConfig::CaptureSource::PRIMARY_OUTPUT:
                ss << "PRIMARY_OUTPUT";
                break;
            case VirtualOutputConfig::CaptureSource::COMPOSITE_FBO:
                ss << "COMPOSITE_FBO";
                break;
            case VirtualOutputConfig::CaptureSource::SPECIFIC_OUTPUT:
                ss << "SPECIFIC_OUTPUT";
                break;
        }
        ss << "\"\n";
        ss << "        }\n";
        ss << "    }";
    }
    
    ss << "\n}\n";
    return ss.str();
}

} // namespace videocomposer

