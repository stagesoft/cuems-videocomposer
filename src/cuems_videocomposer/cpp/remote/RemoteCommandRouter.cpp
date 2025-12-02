#include "RemoteCommandRouter.h"
#include "../VideoComposerApplication.h"
#include "../layer/LayerManager.h"
#include "../layer/VideoLayer.h"
#include "../input/VideoFileInput.h"
#include "../sync/MIDISyncSource.h"
#include "../osd/OSDManager.h"
#include "../display/OpenGLRenderer.h"
#include "../utils/Logger.h"  // For LOG_INFO, LOG_WARNING
#include "../utils/SMPTEUtils.h"
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace videocomposer {

RemoteCommandRouter::RemoteCommandRouter(VideoComposerApplication* app, LayerManager* layerManager)
    : app_(app)
    , layerManager_(layerManager)
{
    // Register app-level commands
    registerAppCommand("quit", [this](const std::vector<std::string>& args) {
        return handleQuit(args);
    });
    registerAppCommand("load", [this](const std::vector<std::string>& args) {
        return handleLoad(args);
    });
    registerAppCommand("seek", [this](const std::vector<std::string>& args) {
        return handleSeek(args);
    });
    registerAppCommand("fps", [this](const std::vector<std::string>& args) {
        return handleFPS(args);
    });
    registerAppCommand("offset", [this](const std::vector<std::string>& args) {
        return handleOffset(args);
    });
    registerAppCommand("layer/add", [this](const std::vector<std::string>& args) {
        return handleLayerAdd(args);
    });
    registerAppCommand("layer/remove", [this](const std::vector<std::string>& args) {
        return handleLayerRemove(args);
    });
    registerAppCommand("layer/duplicate", [this](const std::vector<std::string>& args) {
        return handleLayerDuplicate(args);
    });
    registerAppCommand("layer/reorder", [this](const std::vector<std::string>& args) {
        return handleLayerReorder(args);
    });
    registerAppCommand("layer/list", [this](const std::vector<std::string>& args) {
        return handleLayerList(args);
    });
    registerAppCommand("osd/frame", [this](const std::vector<std::string>& args) {
        return handleOSDFrame(args);
    });
    registerAppCommand("osd/smpte", [this](const std::vector<std::string>& args) {
        return handleOSDSMPTE(args);
    });
    registerAppCommand("osd/text", [this](const std::vector<std::string>& args) {
        return handleOSDText(args);
    });
    registerAppCommand("osd/box", [this](const std::vector<std::string>& args) {
        return handleOSDBox(args);
    });
    registerAppCommand("osd/font", [this](const std::vector<std::string>& args) {
        return handleOSDFont(args);
    });
    registerAppCommand("osd/pos", [this](const std::vector<std::string>& args) {
        return handleOSDPos(args);
    });
    registerAppCommand("art/timescale", [this](const std::vector<std::string>& args) {
        if (args.size() >= 2) {
            return handleTimeScale2(args);  // timescale + offset
        } else {
            return handleTimeScale(args);   // timescale only
        }
    });
    registerAppCommand("art/loop", [this](const std::vector<std::string>& args) {
        return handleLoop(args);
    });
    registerAppCommand("art/reverse", [this](const std::vector<std::string>& args) {
        return handleReverse(args);
    });
    registerAppCommand("art/pan", [this](const std::vector<std::string>& args) {
        return handlePan(args);
    });

    // Register layer-level commands
    registerLayerCommand("seek", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerSeek(layer, args);
    });
    registerLayerCommand("play", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerPlay(layer, args);
    });
    registerLayerCommand("pause", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerPause(layer, args);
    });
    registerLayerCommand("position", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerPosition(layer, args);
    });
    registerLayerCommand("opacity", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerOpacity(layer, args);
    });
    registerLayerCommand("visible", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerVisible(layer, args);
    });
    registerLayerCommand("zorder", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerZOrder(layer, args);
    });
    registerLayerCommand("timescale", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        if (args.size() >= 2) {
            return handleLayerTimeScale2(layer, args);  // timescale + offset
        } else {
            return handleLayerTimeScale(layer, args);   // timescale only
        }
    });
    registerLayerCommand("loop", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerLoop(layer, args);
    });
    registerLayerCommand("reverse", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerReverse(layer, args);
    });
    registerLayerCommand("pan", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerPan(layer, args);
    });
    registerLayerCommand("crop", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCrop(layer, args);
    });
    registerLayerCommand("crop/disable", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCropDisable(layer, args);
    });
    registerLayerCommand("panorama", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerPanorama(layer, args);
    });
    registerLayerCommand("file", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerFile(layer, args);
    });
    registerLayerCommand("autounload", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerAutoUnload(layer, args);
    });
    registerLayerCommand("loop/region", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerLoopRegion(layer, args);
    });
    registerLayerCommand("loop/region/disable", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerLoopRegionDisable(layer, args);
    });
    registerLayerCommand("offset", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerOffset(layer, args);
    });
    registerLayerCommand("mtcfollow", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerMtcFollow(layer, args);
    });
    registerLayerCommand("scale", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerScale(layer, args);
    });
    registerLayerCommand("xscale", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerXScale(layer, args);
    });
    registerLayerCommand("yscale", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerYScale(layer, args);
    });
    registerLayerCommand("rotation", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerRotation(layer, args);
    });
    registerLayerCommand("corner_deform", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCornerDeform(layer, args);
    });
    registerLayerCommand("corner_deform_enable", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCornerDeformEnable(layer, args);
    });
    registerLayerCommand("corner_deform_hq", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCornerDeformHQ(layer, args);
    });
    registerLayerCommand("corners", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCorners(layer, args);
    });
    registerLayerCommand("corner1", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCorner1(layer, args);
    });
    registerLayerCommand("corner2", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCorner2(layer, args);
    });
    registerLayerCommand("corner3", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCorner3(layer, args);
    });
    registerLayerCommand("corner4", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerCorner4(layer, args);
    });
    registerLayerCommand("blendmode", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerBlendMode(layer, args);
    });
    
    // Layer color correction commands
    registerLayerCommand("brightness", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerBrightness(layer, args);
    });
    registerLayerCommand("contrast", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerContrast(layer, args);
    });
    registerLayerCommand("saturation", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerSaturation(layer, args);
    });
    registerLayerCommand("hue", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerHue(layer, args);
    });
    registerLayerCommand("gamma", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerGamma(layer, args);
    });
    registerLayerCommand("color/reset", [this](VideoLayer* layer, const std::vector<std::string>& args) {
        return handleLayerColorReset(layer, args);
    });
    
    // Register master layer commands (composite transforms)
    registerAppCommand("master/opacity", [this](const std::vector<std::string>& args) {
        return handleMasterOpacity(args);
    });
    registerAppCommand("master/position", [this](const std::vector<std::string>& args) {
        return handleMasterPosition(args);
    });
    registerAppCommand("master/scale", [this](const std::vector<std::string>& args) {
        return handleMasterScale(args);
    });
    registerAppCommand("master/xscale", [this](const std::vector<std::string>& args) {
        return handleMasterXScale(args);
    });
    registerAppCommand("master/yscale", [this](const std::vector<std::string>& args) {
        return handleMasterYScale(args);
    });
    registerAppCommand("master/rotation", [this](const std::vector<std::string>& args) {
        return handleMasterRotation(args);
    });
    registerAppCommand("master/corners", [this](const std::vector<std::string>& args) {
        return handleMasterCorners(args);
    });
    registerAppCommand("master/corner1", [this](const std::vector<std::string>& args) {
        return handleMasterCorner1(args);
    });
    registerAppCommand("master/corner2", [this](const std::vector<std::string>& args) {
        return handleMasterCorner2(args);
    });
    registerAppCommand("master/corner3", [this](const std::vector<std::string>& args) {
        return handleMasterCorner3(args);
    });
    registerAppCommand("master/corner4", [this](const std::vector<std::string>& args) {
        return handleMasterCorner4(args);
    });
    registerAppCommand("master/reset", [this](const std::vector<std::string>& args) {
        return handleMasterReset(args);
    });
    
    // Master color correction commands
    registerAppCommand("master/brightness", [this](const std::vector<std::string>& args) {
        return handleMasterBrightness(args);
    });
    registerAppCommand("master/contrast", [this](const std::vector<std::string>& args) {
        return handleMasterContrast(args);
    });
    registerAppCommand("master/saturation", [this](const std::vector<std::string>& args) {
        return handleMasterSaturation(args);
    });
    registerAppCommand("master/hue", [this](const std::vector<std::string>& args) {
        return handleMasterHue(args);
    });
    registerAppCommand("master/gamma", [this](const std::vector<std::string>& args) {
        return handleMasterGamma(args);
    });
    registerAppCommand("master/color/reset", [this](const std::vector<std::string>& args) {
        return handleMasterColorReset(args);
    });
    
    // Display configuration commands (Phase 4)
    registerAppCommand("display/list", [this](const std::vector<std::string>& args) {
        return handleDisplayList(args);
    });
    registerAppCommand("display/mode", [this](const std::vector<std::string>& args) {
        return handleDisplayMode(args);
    });
    registerAppCommand("display/assign", [this](const std::vector<std::string>& args) {
        return handleDisplayAssign(args);
    });
    registerAppCommand("display/blend", [this](const std::vector<std::string>& args) {
        return handleDisplayBlend(args);
    });
    registerAppCommand("display/save", [this](const std::vector<std::string>& args) {
        return handleDisplaySave(args);
    });
    registerAppCommand("display/load", [this](const std::vector<std::string>& args) {
        return handleDisplayLoad(args);
    });
}

RemoteCommandRouter::~RemoteCommandRouter() {
}

bool RemoteCommandRouter::routeCommand(const std::string& path, const std::vector<std::string>& args) {
    // Handle /videocomposer/cmd - remote command interface (takes a string command)
    if (path == "/videocomposer/cmd" && !args.empty()) {
        // Parse the command string (e.g., "osd smpte 89")
        std::string cmd = args[0];
        size_t firstSpace = cmd.find(' ');
        std::string cmdPath = (firstSpace != std::string::npos) ? cmd.substr(0, firstSpace) : cmd;
        std::vector<std::string> cmdArgs;
        
        // Parse remaining arguments
        size_t pos = firstSpace;
        while (pos != std::string::npos && pos < cmd.length()) {
            pos = cmd.find_first_not_of(' ', pos);
            if (pos == std::string::npos) break;
            size_t end = cmd.find(' ', pos);
            if (end == std::string::npos) {
                cmdArgs.push_back(cmd.substr(pos));
                break;
            } else {
                cmdArgs.push_back(cmd.substr(pos, end - pos));
                pos = end;
            }
        }
        
        // Route the parsed command
        return routeCommand(cmdPath, cmdArgs);
    }
    
    // Remove leading /videocomposer/ if present
    std::string cleanPath = path;
    if (cleanPath.find("/videocomposer/") == 0) {
        cleanPath = cleanPath.substr(15); // Remove "/videocomposer/"
    } else if (cleanPath.find("/videocomposer") == 0) {
        cleanPath = cleanPath.substr(14); // Remove "/videocomposer"
    }
    
    // Log only in verbose mode to avoid blocking on high message rates
    // LOG_VERBOSE << "OSC: Routing command: path='" << path << "' cleanPath='" << cleanPath << "' args.size()=" << args.size();

    // Check if it's a layer-level command
    if (cleanPath.find("layer/") == 0) {
        std::string remaining = cleanPath.substr(6); // Remove "layer/"
        
        // Check for special commands: load, unload
        if (remaining == "load") {
            // /videocomposer/layer/load s s (filepath, cueId)
            return handleLayerLoad(args);
        } else if (remaining == "unload") {
            // /videocomposer/layer/unload s (cueId)
            return handleLayerUnload(args);
        }
        
        // Parse layer ID (UUID string) and command
        std::string cueId;
        std::string command;
        
        size_t slashPos = remaining.find('/');
        if (slashPos != std::string::npos) {
            cueId = remaining.substr(0, slashPos);
            command = remaining.substr(slashPos + 1);
            
            // Try to get layer by cue ID (UUID)
            VideoLayer* layer = layerManager_->getLayerByCueId(cueId);
            if (layer) {
                // Layer found by UUID - route to layer command
                auto it = layerCommands_.find(command);
                if (it != layerCommands_.end()) {
                    return it->second(layer, args);
                }
            } else {
                // Try integer layer ID for backward compatibility
                int layerId = std::atoi(cueId.c_str());
                if (layerId >= 0) {
                    layer = layerManager_->getLayer(layerId);
                    if (layer) {
            auto it = layerCommands_.find(command);
            if (it != layerCommands_.end()) {
                return it->second(layer, args);
                        }
                    }
                }
            }
        } else {
            // No layer ID specified - treat as app-level layer command
            command = remaining;
            auto it = appCommands_.find("layer/" + command);
            if (it != appCommands_.end()) {
                return it->second(args);
            }
        }
    } else {
        // App-level command
        auto it = appCommands_.find(cleanPath);
        if (it != appCommands_.end()) {
            return it->second(args);
        }
    }

    return false;
}

void RemoteCommandRouter::registerAppCommand(const std::string& path,
                                             std::function<bool(const std::vector<std::string>&)> handler) {
    appCommands_[path] = handler;
}

void RemoteCommandRouter::registerLayerCommand(const std::string& path,
                                               std::function<bool(VideoLayer*, const std::vector<std::string>&)> handler) {
    layerCommands_[path] = handler;
}

bool RemoteCommandRouter::parsePath(const std::string& path, std::string& command, int& layerId) {
    // This is handled in routeCommand now
    return false;
}

// App-level command handlers
bool RemoteCommandRouter::handleQuit(const std::vector<std::string>& args) {
    if (app_) {
        app_->quit();
    }
    return true;
}

bool RemoteCommandRouter::handleLoad(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    // Create new layer with video file
    // This would be implemented to create a layer via VideoComposerApplication
    // For now, placeholder
    return true;
}

bool RemoteCommandRouter::handleSeek(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    int64_t frame = std::atoll(args[0].c_str());
    
    // Seek all layers or first layer
    if (layerManager_) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            return layers[0]->seek(frame);
        }
    }
    
    return false;
}

bool RemoteCommandRouter::handleFPS(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    double fps = std::atof(args[0].c_str());
    return app_->setFPS(fps);
}

bool RemoteCommandRouter::handleOffset(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    int64_t offset = std::atoll(args[0].c_str());
    return app_->setTimeOffset(offset);
}

bool RemoteCommandRouter::handleLayerAdd(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    // Create new layer
    // This would be implemented via VideoComposerApplication
    return true;
}

bool RemoteCommandRouter::handleLayerRemove(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    int layerId = std::atoi(args[0].c_str());
    if (layerManager_) {
        return layerManager_->removeLayer(layerId);
    }
    return false;
}

bool RemoteCommandRouter::handleLayerDuplicate(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }

    int layerId = std::atoi(args[0].c_str());
    if (layerManager_) {
        int newLayerId = -1;
        if (layerManager_->duplicateLayer(layerId, &newLayerId)) {
            // Could send response with new layer ID
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleLayerReorder(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return false;
    }

    int layerId = std::atoi(args[0].c_str());
    std::string action = args[1];
    
    if (layerManager_) {
        if (action == "top" || action == "front") {
            return layerManager_->moveLayerToTop(layerId);
        } else if (action == "bottom" || action == "back") {
            return layerManager_->moveLayerToBottom(layerId);
        } else if (action == "up") {
            return layerManager_->moveLayerUp(layerId);
        } else if (action == "down") {
            return layerManager_->moveLayerDown(layerId);
        } else {
            // Try to set specific z-order
            int zOrder = std::atoi(action.c_str());
            return layerManager_->setLayerZOrder(layerId, zOrder);
        }
    }
    return false;
}

bool RemoteCommandRouter::handleLayerList(const std::vector<std::string>& args) {
    if (!layerManager_) {
        return false;
    }

    // Get all layers sorted by z-order
    auto layers = layerManager_->getLayersSortedByZOrder();
    
    // Output layer list (for now, just return success)
    // In a full implementation, this would send OSC responses or use a callback
    // to report layer information
    
    return true;
}

// Layer-level command handlers
bool RemoteCommandRouter::handleLayerSeek(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }

    int64_t frame = std::atoll(args[0].c_str());
    return layer->seek(frame);
}

bool RemoteCommandRouter::handleLayerPlay(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    return layer->play();
}

bool RemoteCommandRouter::handleLayerPause(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    return layer->pause();
}

bool RemoteCommandRouter::handleLayerPosition(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }

    auto& props = layer->properties();
    // Support both integer and float positions for smooth movement
    // Try parsing as float first (for smooth sub-pixel positioning)
    float x = std::atof(args[0].c_str());
    float y = std::atof(args[1].c_str());
    props.x = static_cast<int>(x);
    props.y = static_cast<int>(y);
    // Store sub-pixel precision if needed (for future interpolation)
    // For now, we use integer positions but accept float input for smooth updates
    return true;
}

bool RemoteCommandRouter::handleLayerOpacity(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }

    float opacity = std::atof(args[0].c_str());
    opacity = std::max(0.0f, std::min(1.0f, opacity)); // Clamp to 0-1
    layer->properties().opacity = opacity;
    return true;
}

bool RemoteCommandRouter::handleLayerVisible(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }

    int visible = std::atoi(args[0].c_str());
    layer->properties().visible = (visible != 0);
    return true;
}

bool RemoteCommandRouter::handleLayerZOrder(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }

    int zOrder = std::atoi(args[0].c_str());
    if (layerManager_) {
        return layerManager_->setLayerZOrder(layer->getLayerId(), zOrder);
    }
    return false;
}

bool RemoteCommandRouter::handleOSDFrame(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    if (args.empty()) {
        // Toggle frame display
        if (osd->isModeEnabled(OSDManager::FRAME)) {
            osd->disableMode(OSDManager::FRAME);
        } else {
            osd->enableMode(OSDManager::FRAME);
        }
    } else {
        int yPos = std::atoi(args[0].c_str());
        if (yPos < 0) {
            osd->disableMode(OSDManager::FRAME);
        } else if (yPos <= 100) {
            osd->enableMode(OSDManager::FRAME);
            // Note: BOX is not automatically enabled (matches xjadeo behavior - BOX is independent toggle)
            osd->setFramePosition(1, yPos); // Center aligned by default
        } else {
            return false;
        }
    }
    return true;
}

bool RemoteCommandRouter::handleOSDSMPTE(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    if (args.empty()) {
        // Toggle SMPTE display
        if (osd->isModeEnabled(OSDManager::SMPTE)) {
            osd->disableMode(OSDManager::SMPTE);
        } else {
            osd->enableMode(OSDManager::SMPTE);
        }
    } else {
        int yPos = std::atoi(args[0].c_str());
        if (yPos < 0) {
            osd->disableMode(OSDManager::SMPTE);
        } else if (yPos <= 100) {
            osd->enableMode(OSDManager::SMPTE);
            // Note: BOX is not automatically enabled (matches xjadeo behavior - BOX is independent toggle)
            osd->setSMPTEPosition(1, yPos); // Center aligned by default
        } else {
            return false;
        }
    }
    return true;
}

bool RemoteCommandRouter::handleOSDText(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    if (args.empty()) {
        // Toggle text display
        if (osd->isModeEnabled(OSDManager::TEXT)) {
            osd->disableMode(OSDManager::TEXT);
            LOG_INFO << "OSD: TEXT disabled";
        } else {
            osd->enableMode(OSDManager::TEXT);
            LOG_INFO << "OSD: TEXT enabled (text: '" << osd->getText() << "')";
        }
    } else {
        // Set text
        std::string text;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) text += " ";
            text += args[i];
        }
        osd->setText(text);
        LOG_INFO << "OSD: Text set to: '" << text << "'";
    }
    return true;
}

bool RemoteCommandRouter::handleOSDBox(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    if (args.empty()) {
        // Toggle box
        if (osd->isModeEnabled(OSDManager::BOX)) {
            osd->disableMode(OSDManager::BOX);
            LOG_INFO << "OSD: BOX disabled";
        } else {
            osd->enableMode(OSDManager::BOX);
            LOG_INFO << "OSD: BOX enabled";
        }
    } else {
        int enable = std::atoi(args[0].c_str());
        if (enable) {
            osd->enableMode(OSDManager::BOX);
            LOG_INFO << "OSD: BOX enabled (via arg)";
        } else {
            osd->disableMode(OSDManager::BOX);
            LOG_INFO << "OSD: BOX disabled (via arg)";
        }
    }
    return true;
}

bool RemoteCommandRouter::handleOSDFont(const std::vector<std::string>& args) {
    if (!app_ || args.empty()) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    osd->setFontFile(args[0]);
    return true;
}

bool RemoteCommandRouter::handleOSDPos(const std::vector<std::string>& args) {
    if (!app_ || args.size() < 2) {
        return false;
    }
    
    OSDManager* osd = app_->getOSDManager();
    if (!osd) {
        return false;
    }
    
    int xAlign = std::atoi(args[0].c_str()); // 0=left, 1=center, 2=right
    int yPos = std::atoi(args[1].c_str());    // 0-100
    
    if (xAlign < 0 || xAlign > 2 || yPos < 0 || yPos > 100) {
        return false;
    }
    
    osd->setTextPosition(xAlign, yPos);
    return true;
}

bool RemoteCommandRouter::handleTimeScale(const std::vector<std::string>& args) {
    if (!app_ || args.empty()) {
        return false;
    }
    
    // Apply to first layer (app-level command)
    if (layerManager_ && layerManager_->getLayerCount() > 0) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            double scale = std::atof(args[0].c_str());
            layers[0]->setTimeScale(scale);
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleTimeScale2(const std::vector<std::string>& args) {
    if (!app_ || args.size() < 2) {
        return false;
    }
    
    // Apply to first layer (app-level command)
    if (layerManager_ && layerManager_->getLayerCount() > 0) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            double scale = std::atof(args[0].c_str());
            int64_t offset = std::atoll(args[1].c_str());
            layers[0]->setTimeScale(scale);
            layers[0]->setTimeOffset(offset);
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleLoop(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    // Apply to first layer (app-level command)
    if (layerManager_ && layerManager_->getLayerCount() > 0) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            bool enabled = false;
            if (!args.empty()) {
                int val = std::atoi(args[0].c_str());
                enabled = (val != 0);
            }
            layers[0]->setWraparound(enabled);
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleReverse(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    // Apply to first layer (app-level command)
    if (layerManager_ && layerManager_->getLayerCount() > 0) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            layers[0]->reverse();
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleLayerTimeScale(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    double scale = std::atof(args[0].c_str());
    layer->setTimeScale(scale);
    return true;
}

bool RemoteCommandRouter::handleLayerTimeScale2(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    double scale = std::atof(args[0].c_str());
    int64_t offset = std::atoll(args[1].c_str());
    layer->setTimeScale(scale);
    layer->setTimeOffset(offset);
    return true;
}

bool RemoteCommandRouter::handleLayerLoop(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    auto& props = layer->properties();
    
    // Handle loop count if provided (second argument)
    if (args.size() >= 2) {
        int loopCount = std::atoi(args[1].c_str());
        props.fullFileLoopCount = loopCount;
        props.currentFullFileLoopCount = (loopCount > 0) ? loopCount : -1;
    }
    
    // Handle enable/disable (first argument)
    bool enabled = false;
    if (!args.empty()) {
        int val = std::atoi(args[0].c_str());
        enabled = (val != 0);
    }
    layer->setWraparound(enabled);
    
    // If disabling, also reset loop counts
    if (!enabled) {
        props.fullFileLoopCount = 0;
        props.currentFullFileLoopCount = -1;
    }
    
    return true;
}

bool RemoteCommandRouter::handleLayerReverse(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    layer->reverse();
    return true;
}

bool RemoteCommandRouter::handlePan(const std::vector<std::string>& args) {
    if (!app_ || args.empty()) {
        return false;
    }
    
    // Apply to first layer (app-level command)
    if (layerManager_ && layerManager_->getLayerCount() > 0) {
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            int panOffset = std::atoi(args[0].c_str());
            auto& props = layers[0]->properties();
            
            // Enable panorama mode if not already enabled
            if (!props.panoramaMode) {
                props.panoramaMode = true;
            }
            
            // Clamp pan offset to valid range
            FrameInfo info = layers[0]->getFrameInfo();
            int maxOffset = info.width / 2; // 50% of width in panorama mode
            if (panOffset < 0) panOffset = 0;
            if (panOffset > info.width - maxOffset) panOffset = info.width - maxOffset;
            
            props.panOffset = panOffset;
            return true;
        }
    }
    return false;
}

bool RemoteCommandRouter::handleLayerPan(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    int panOffset = std::atoi(args[0].c_str());
    auto& props = layer->properties();
    
    // Enable panorama mode if not already enabled
    if (!props.panoramaMode) {
        props.panoramaMode = true;
    }
    
    // Clamp pan offset to valid range
    FrameInfo info = layer->getFrameInfo();
    int maxOffset = info.width / 2; // 50% of width in panorama mode
    if (panOffset < 0) panOffset = 0;
    if (panOffset > info.width - maxOffset) panOffset = info.width - maxOffset;
    
    props.panOffset = panOffset;
    return true;
}

bool RemoteCommandRouter::handleLayerCrop(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    auto& props = layer->properties();
    
    if (args.empty()) {
        // Toggle crop
        props.crop.enabled = !props.crop.enabled;
    } else if (args.size() >= 4) {
        // Set crop rectangle: x, y, width, height
        props.crop.x = std::atoi(args[0].c_str());
        props.crop.y = std::atoi(args[1].c_str());
        props.crop.width = std::atoi(args[2].c_str());
        props.crop.height = std::atoi(args[3].c_str());
        props.crop.enabled = true;
    } else {
        return false;
    }
    
    return true;
}

bool RemoteCommandRouter::handleLayerCropDisable(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    layer->properties().crop.enabled = false;
    return true;
}

bool RemoteCommandRouter::handleLayerPanorama(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    auto& props = layer->properties();
    
    if (args.empty()) {
        // Toggle panorama mode
        props.panoramaMode = !props.panoramaMode;
    } else {
        // Enable/disable based on argument
        int enabled = std::atoi(args[0].c_str());
        props.panoramaMode = (enabled != 0);
    }
    
    return true;
}

bool RemoteCommandRouter::handleLayerLoad(const std::vector<std::string>& args) {
    if (!app_ || args.size() < 2) {
        return false;
    }
    
    std::string filepath = args[0];
    std::string cueId = args[1];
    
    return app_->createLayerWithFile(cueId, filepath);
}

bool RemoteCommandRouter::handleLayerFile(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || !app_ || args.empty()) {
        return false;
    }
    
    std::string filepath = args[0];
    
    // Get cue ID from layer manager
    std::string cueId = layerManager_->getCueIdFromLayer(layer);
    if (cueId.empty()) {
        LOG_WARNING << "Could not find cue ID for layer";
        return false;
    }
    
    return app_->loadFileIntoLayer(cueId, filepath);
}

bool RemoteCommandRouter::handleLayerUnload(const std::vector<std::string>& args) {
    if (!app_ || args.empty()) {
        return false;
    }
    
    std::string cueId = args[0];
    return app_->unloadFileFromLayer(cueId);
}

bool RemoteCommandRouter::handleLayerAutoUnload(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    int enabled = std::atoi(args[0].c_str());
    layer->properties().autoUnload = (enabled != 0);
    return true;
}

bool RemoteCommandRouter::handleLayerLoopRegion(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.loopRegion.startFrame = std::atoll(args[0].c_str());
    props.loopRegion.endFrame = std::atoll(args[1].c_str());
    props.loopRegion.enabled = true;
    
    // Set loop count if provided (third argument)
    if (args.size() >= 3) {
        props.loopRegion.loopCount = std::atoi(args[2].c_str());
    } else {
        props.loopRegion.loopCount = -1; // Infinite
    }
    
    // Initialize current loop count
    props.loopRegion.currentLoopCount = props.loopRegion.loopCount;
    
    return true;
}

bool RemoteCommandRouter::handleLayerLoopRegionDisable(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    layer->properties().loopRegion.enabled = false;
    return true;
}


bool RemoteCommandRouter::handleLayerOffset(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    // Check if it's SMPTE timecode string or integer frame
    std::string offsetStr = args[0];
    int64_t offset = 0;
    
    if (offsetStr.find(':') != std::string::npos || offsetStr.find(';') != std::string::npos) {
        // SMPTE timecode string - convert to frames
        FrameInfo info = layer->getFrameInfo();
        double framerate = info.framerate;
        if (framerate <= 0) {
            framerate = 25.0; // Default framerate if unknown
        }
        
        // Detect drop-frame from separator
        bool haveDropframes = (offsetStr.find(';') != std::string::npos);
        
        offset = SMPTEUtils::smpteStringToFrame(offsetStr, framerate, haveDropframes, false, true);
    } else {
        // Integer frame offset
        offset = std::atoll(offsetStr.c_str());
    }
    
    layer->setTimeOffset(offset);
    return true;
}

bool RemoteCommandRouter::handleLayerMtcFollow(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    int enabled = std::atoi(args[0].c_str());
    layer->setMtcFollow(enabled != 0);
    return true;
}

bool RemoteCommandRouter::handleLayerScale(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.scaleX = std::atof(args[0].c_str());
    props.scaleY = std::atof(args[1].c_str());
    return true;
}

bool RemoteCommandRouter::handleLayerXScale(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    layer->properties().scaleX = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleLayerYScale(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    layer->properties().scaleY = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleLayerRotation(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    layer->properties().rotation = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleLayerCornerDeform(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 8) {
        return false;
    }
    
    auto& props = layer->properties();
    // Corner deform expects 8 float values: 4 corners * (x_offset, y_offset)
    // Format: corner0_x, corner0_y, corner1_x, corner1_y, corner2_x, corner2_y, corner3_x, corner3_y
    for (int i = 0; i < 8; i++) {
        props.cornerDeform.corners[i] = std::atof(args[i].c_str());
    }
    return true;
}

bool RemoteCommandRouter::handleLayerCornerDeformEnable(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    layer->properties().cornerDeform.enabled = (std::atoi(args[0].c_str()) != 0);
    return true;
}

bool RemoteCommandRouter::handleLayerCornerDeformHQ(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    layer->properties().cornerDeform.highQuality = (std::atoi(args[0].c_str()) != 0);
    return true;
}

bool RemoteCommandRouter::handleLayerCorners(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 8) {
        return false;
    }
    
    auto& props = layer->properties();
    for (int i = 0; i < 8; ++i) {
        props.cornerDeform.corners[i] = std::atof(args[i].c_str());
    }
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleLayerCorner1(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.cornerDeform.corners[0] = std::atof(args[0].c_str());
    props.cornerDeform.corners[1] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleLayerCorner2(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.cornerDeform.corners[2] = std::atof(args[0].c_str());
    props.cornerDeform.corners[3] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleLayerCorner3(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.cornerDeform.corners[4] = std::atof(args[0].c_str());
    props.cornerDeform.corners[5] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleLayerCorner4(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.size() < 2) {
        return false;
    }
    
    auto& props = layer->properties();
    props.cornerDeform.corners[6] = std::atof(args[0].c_str());
    props.cornerDeform.corners[7] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleLayerBlendMode(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    int mode = std::atoi(args[0].c_str());
    auto& props = layer->properties();
    
    switch (mode) {
        case 0:
            props.blendMode = LayerProperties::NORMAL;
            break;
        case 1:
            props.blendMode = LayerProperties::MULTIPLY;
            break;
        case 2:
            props.blendMode = LayerProperties::SCREEN;
            break;
        case 3:
            props.blendMode = LayerProperties::OVERLAY;
            break;
        default:
            return false;
    }
    
    return true;
}

// Master layer handlers (composite transforms)
bool RemoteCommandRouter::handleMasterOpacity(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float opacity = std::atof(args[0].c_str());
    app_->renderer().masterProperties().opacity = std::max(0.0f, std::min(1.0f, opacity));
    return true;
}

bool RemoteCommandRouter::handleMasterPosition(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.x = std::atof(args[0].c_str());
    props.y = std::atof(args[1].c_str());
    return true;
}

bool RemoteCommandRouter::handleMasterScale(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.scaleX = std::atof(args[0].c_str());
    props.scaleY = std::atof(args[1].c_str());
    return true;
}

bool RemoteCommandRouter::handleMasterXScale(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    app_->renderer().masterProperties().scaleX = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleMasterYScale(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    app_->renderer().masterProperties().scaleY = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleMasterRotation(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    app_->renderer().masterProperties().rotation = std::atof(args[0].c_str());
    return true;
}

bool RemoteCommandRouter::handleMasterCorners(const std::vector<std::string>& args) {
    if (args.size() < 8 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    for (int i = 0; i < 8; ++i) {
        props.cornerDeform.corners[i] = std::atof(args[i].c_str());
    }
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleMasterCorner1(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.cornerDeform.corners[0] = std::atof(args[0].c_str());
    props.cornerDeform.corners[1] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleMasterCorner2(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.cornerDeform.corners[2] = std::atof(args[0].c_str());
    props.cornerDeform.corners[3] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleMasterCorner3(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.cornerDeform.corners[4] = std::atof(args[0].c_str());
    props.cornerDeform.corners[5] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleMasterCorner4(const std::vector<std::string>& args) {
    if (args.size() < 2 || !app_) {
        return false;
    }
    
    auto& props = app_->renderer().masterProperties();
    props.cornerDeform.corners[6] = std::atof(args[0].c_str());
    props.cornerDeform.corners[7] = std::atof(args[1].c_str());
    props.cornerDeform.enabled = true;
    return true;
}

bool RemoteCommandRouter::handleMasterReset(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    app_->renderer().masterProperties().reset();
    return true;
}

// Layer color correction handlers
bool RemoteCommandRouter::handleLayerBrightness(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    float brightness = std::atof(args[0].c_str());
    brightness = std::max(-1.0f, std::min(1.0f, brightness));  // Clamp to -1 to 1
    layer->properties().colorAdjust.brightness = brightness;
    return true;
}

bool RemoteCommandRouter::handleLayerContrast(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    float contrast = std::atof(args[0].c_str());
    contrast = std::max(0.0f, std::min(2.0f, contrast));  // Clamp to 0 to 2
    layer->properties().colorAdjust.contrast = contrast;
    return true;
}

bool RemoteCommandRouter::handleLayerSaturation(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    float saturation = std::atof(args[0].c_str());
    saturation = std::max(0.0f, std::min(2.0f, saturation));  // Clamp to 0 to 2
    layer->properties().colorAdjust.saturation = saturation;
    return true;
}

bool RemoteCommandRouter::handleLayerHue(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    float hue = std::atof(args[0].c_str());
    // Wrap hue to -180 to 180
    while (hue > 180.0f) hue -= 360.0f;
    while (hue < -180.0f) hue += 360.0f;
    layer->properties().colorAdjust.hue = hue;
    return true;
}

bool RemoteCommandRouter::handleLayerGamma(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer || args.empty()) {
        return false;
    }
    
    float gamma = std::atof(args[0].c_str());
    gamma = std::max(0.1f, std::min(3.0f, gamma));  // Clamp to 0.1 to 3
    layer->properties().colorAdjust.gamma = gamma;
    return true;
}

bool RemoteCommandRouter::handleLayerColorReset(VideoLayer* layer, const std::vector<std::string>& args) {
    if (!layer) {
        return false;
    }
    
    layer->properties().colorAdjust.reset();
    return true;
}

// Master color correction handlers
bool RemoteCommandRouter::handleMasterBrightness(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float brightness = std::atof(args[0].c_str());
    brightness = std::max(-1.0f, std::min(1.0f, brightness));
    app_->renderer().masterProperties().colorAdjust.brightness = brightness;
    return true;
}

bool RemoteCommandRouter::handleMasterContrast(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float contrast = std::atof(args[0].c_str());
    contrast = std::max(0.0f, std::min(2.0f, contrast));
    app_->renderer().masterProperties().colorAdjust.contrast = contrast;
    return true;
}

bool RemoteCommandRouter::handleMasterSaturation(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float saturation = std::atof(args[0].c_str());
    saturation = std::max(0.0f, std::min(2.0f, saturation));
    app_->renderer().masterProperties().colorAdjust.saturation = saturation;
    return true;
}

bool RemoteCommandRouter::handleMasterHue(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float hue = std::atof(args[0].c_str());
    while (hue > 180.0f) hue -= 360.0f;
    while (hue < -180.0f) hue += 360.0f;
    app_->renderer().masterProperties().colorAdjust.hue = hue;
    return true;
}

bool RemoteCommandRouter::handleMasterGamma(const std::vector<std::string>& args) {
    if (args.empty() || !app_) {
        return false;
    }
    
    float gamma = std::atof(args[0].c_str());
    gamma = std::max(0.1f, std::min(3.0f, gamma));
    app_->renderer().masterProperties().colorAdjust.gamma = gamma;
    return true;
}

bool RemoteCommandRouter::handleMasterColorReset(const std::vector<std::string>& args) {
    if (!app_) {
        return false;
    }
    
    app_->renderer().masterProperties().colorAdjust.reset();
    return true;
}

// ============================================================================
// Display Configuration Handlers (Phase 4)
// ============================================================================

bool RemoteCommandRouter::handleDisplayList(const std::vector<std::string>& args) {
    (void)args;  // No arguments needed
    
    // TODO: When DisplayConfiguration is integrated into VideoComposerApplication,
    // this will return a JSON-formatted list of all outputs with their metadata.
    // For now, log a message indicating the feature is available.
    LOG_INFO << "Display list requested - feature pending full integration";
    
    // Return true to indicate command was recognized
    return true;
}

bool RemoteCommandRouter::handleDisplayMode(const std::vector<std::string>& args) {
    // Expected: /videocomposer/display/mode <name> <width> <height> <refresh>
    if (args.size() < 4) {
        LOG_WARNING << "display/mode requires: <name> <width> <height> <refresh>";
        return false;
    }
    
    std::string name = args[0];
    int width = std::atoi(args[1].c_str());
    int height = std::atoi(args[2].c_str());
    double refresh = std::atof(args[3].c_str());
    
    LOG_INFO << "Display mode change requested: " << name 
             << " to " << width << "x" << height << "@" << refresh << "Hz"
             << " - feature pending full integration";
    
    return true;
}

bool RemoteCommandRouter::handleDisplayAssign(const std::vector<std::string>& args) {
    // Expected: /videocomposer/display/assign <layerId> <outputName>
    if (args.size() < 2) {
        LOG_WARNING << "display/assign requires: <layerId> <outputName>";
        return false;
    }
    
    int layerId = std::atoi(args[0].c_str());
    std::string outputName = args[1];
    
    LOG_INFO << "Layer " << layerId << " assigned to output " << outputName
             << " - feature pending full integration";
    
    return true;
}

bool RemoteCommandRouter::handleDisplayBlend(const std::vector<std::string>& args) {
    // Expected: /videocomposer/display/blend <outputName> <left> <right> <top> <bottom>
    if (args.size() < 5) {
        LOG_WARNING << "display/blend requires: <outputName> <left> <right> <top> <bottom>";
        return false;
    }
    
    std::string outputName = args[0];
    float left = std::atof(args[1].c_str());
    float right = std::atof(args[2].c_str());
    float top = std::atof(args[3].c_str());
    float bottom = std::atof(args[4].c_str());
    
    LOG_INFO << "Blend region for " << outputName 
             << ": L=" << left << " R=" << right 
             << " T=" << top << " B=" << bottom
             << " - feature pending full integration";
    
    return true;
}

bool RemoteCommandRouter::handleDisplaySave(const std::vector<std::string>& args) {
    // Expected: /videocomposer/display/save <path>
    if (args.empty()) {
        LOG_WARNING << "display/save requires: <path>";
        return false;
    }
    
    std::string path = args[0];
    
    LOG_INFO << "Display config save to " << path
             << " - feature pending full integration";
    
    return true;
}

bool RemoteCommandRouter::handleDisplayLoad(const std::vector<std::string>& args) {
    // Expected: /videocomposer/display/load <path>
    if (args.empty()) {
        LOG_WARNING << "display/load requires: <path>";
        return false;
    }
    
    std::string path = args[0];
    
    LOG_INFO << "Display config load from " << path
             << " - feature pending full integration";
    
    return true;
}

} // namespace videocomposer

