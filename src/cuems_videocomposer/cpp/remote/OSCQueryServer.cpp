#ifdef HAVE_OSCQUERY

#include "OSCQueryServer.h"
#include "../VideoComposerApplication.h"
#include "../layer/LayerManager.h"
#include "../layer/VideoLayer.h"
#include "../layer/LayerProperties.h"
#include "../osd/OSDManager.h"
#include "../display/OpenGLRenderer.h"
#include "../display/MasterProperties.h"
#include "../remote/RemoteCommandRouter.h"
#include "../utils/Logger.h"
#include <ossia/network/oscquery/oscquery_server.hpp>
#include <ossia/network/base/parameter_data.hpp>
#include <ossia/network/common/complex_type.hpp>
#include <sstream>
#include <algorithm>

namespace videocomposer {

using namespace ossia::net;
using ossia::access_mode;
using ossia::make_domain;

OSCQueryServer::OSCQueryServer(VideoComposerApplication* app, LayerManager* layerManager)
    : app_(app)
    , layerManager_(layerManager)
    , active_(false)
    , oscPort_(7000)
    , wsPort_(7001)
{
}

OSCQueryServer::~OSCQueryServer() {
    shutdown();
}

bool OSCQueryServer::initialize(int oscPort, int wsPort) {
    if (active_) {
        shutdown();
    }
    
    oscPort_ = oscPort;
    wsPort_ = wsPort;
    
    try {
        // Create OSCQuery server protocol
        auto protocol = std::make_unique<ossia::oscquery::oscquery_server_protocol>(oscPort_, wsPort_);
        
        // Create device
        device_ = std::make_unique<generic_device>(std::move(protocol), "cuems-videocomposer");
        
        // Register all parameters
        registerApplicationParameters();
        registerMasterParameters();
        registerOSDParameters();
        
        // Register existing layers
        if (layerManager_) {
            auto layers = layerManager_->getLayers();
            for (auto* layer : layers) {
                if (layer) {
                    registerLayerParameters(layer->getLayerId());
                }
            }
        }
        
        active_ = true;
        LOG_INFO << "OSCQuery server initialized on OSC port " << oscPort_ << ", WebSocket port " << wsPort_;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to initialize OSCQuery server: " << e.what();
        return false;
    }
}

void OSCQueryServer::shutdown() {
    if (!active_) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear all parameters
    parameters_.clear();
    layerParameterPaths_.clear();
    
    // Destroy device
    device_.reset();
    
    active_ = false;
    LOG_INFO << "OSCQuery server shut down";
}

bool OSCQueryServer::isActive() const {
    return active_ && device_ != nullptr;
}

ossia::net::node_base& OSCQueryServer::findOrCreateNode(const std::string& path) {
    return ossia::net::find_or_create_node(*device_, path);
}

void OSCQueryServer::registerApplicationParameters() {
    if (!device_ || !app_) {
        return;
    }
    
    // /videocomposer/quit - Impulse
    {
        auto& node = findOrCreateNode("/videocomposer/quit");
        auto param = node.create_parameter(ossia::val_type::IMPULSE);
        node.set(description_attribute{}, "Quit the application");
        node.set(access_mode_attribute{}, access_mode::SET);
        
        param->add_callback([this](const ossia::value&) {
            if (app_) {
                app_->quit();
            }
        });
        
        parameters_["/videocomposer/quit"] = param;
    }
    
    // /videocomposer/fps - Float [1.0-120.0]
    {
        auto& node = findOrCreateNode("/videocomposer/fps");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(1.0, 120.0));
        node.set(description_attribute{}, "Application framerate (1.0-120.0)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 30.0);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->setFPS(v.get<float>());
            }
        });
        
        parameters_["/videocomposer/fps"] = param;
    }
    
    // /videocomposer/offset - Int64 (frames)
    {
        auto& node = findOrCreateNode("/videocomposer/offset");
        auto param = node.create_parameter(ossia::val_type::INT);
        node.set(description_attribute{}, "Global time offset in frames");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::INT) {
                app_->setTimeOffset(v.get<int>());
            }
        });
        
        parameters_["/videocomposer/offset"] = param;
    }
}

void OSCQueryServer::registerMasterParameters() {
    if (!device_ || !app_) {
        return;
    }
    
    std::string basePath = "/videocomposer/master";
    
    // Master opacity
    {
        auto& node = findOrCreateNode(basePath + "/opacity");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 1.0f));
        node.set(description_attribute{}, "Master opacity (0.0 = transparent, 1.0 = opaque)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().opacity = v.get<float>();
            }
        });
        
        parameters_[basePath + "/opacity"] = param;
    }
    
    // Master position (Vec2f)
    {
        auto& node = findOrCreateNode(basePath + "/position");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        node.set(description_attribute{}, "Master position (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::VEC2F) {
                auto vec = v.get<ossia::vec2f>();
                app_->renderer().masterProperties().x = vec[0];
                app_->renderer().masterProperties().y = vec[1];
            }
        });
        
        parameters_[basePath + "/position"] = param;
    }
    
    // Master scale (Vec2f)
    {
        auto& node = findOrCreateNode(basePath + "/scale");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        node.set(description_attribute{}, "Master scale (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, ossia::vec2f{1.0f, 1.0f});
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::VEC2F) {
                auto vec = v.get<ossia::vec2f>();
                app_->renderer().masterProperties().scaleX = vec[0];
                app_->renderer().masterProperties().scaleY = vec[1];
            }
        });
        
        parameters_[basePath + "/scale"] = param;
    }
    
    // Master rotation
    {
        auto& node = findOrCreateNode(basePath + "/rotation");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 360.0f));
        node.set(description_attribute{}, "Master rotation in degrees (0-360)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().rotation = v.get<float>();
            }
        });
        
        parameters_[basePath + "/rotation"] = param;
    }
    
    // Master brightness
    {
        auto& node = findOrCreateNode(basePath + "/brightness");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(-1.0f, 1.0f));
        node.set(description_attribute{}, "Master brightness (-1.0 to 1.0, 0 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().colorAdjust.brightness = v.get<float>();
            }
        });
        
        parameters_[basePath + "/brightness"] = param;
    }
    
    // Master contrast
    {
        auto& node = findOrCreateNode(basePath + "/contrast");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 2.0f));
        node.set(description_attribute{}, "Master contrast (0.0 to 2.0, 1 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().colorAdjust.contrast = v.get<float>();
            }
        });
        
        parameters_[basePath + "/contrast"] = param;
    }
    
    // Master saturation
    {
        auto& node = findOrCreateNode(basePath + "/saturation");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 2.0f));
        node.set(description_attribute{}, "Master saturation (0.0 to 2.0, 1 = no change, 0 = grayscale)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().colorAdjust.saturation = v.get<float>();
            }
        });
        
        parameters_[basePath + "/saturation"] = param;
    }
    
    // Master hue
    {
        auto& node = findOrCreateNode(basePath + "/hue");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(-180.0f, 180.0f));
        node.set(description_attribute{}, "Master hue shift in degrees (-180 to 180)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().colorAdjust.hue = v.get<float>();
            }
        });
        
        parameters_[basePath + "/hue"] = param;
    }
    
    // Master gamma
    {
        auto& node = findOrCreateNode(basePath + "/gamma");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.1f, 3.0f));
        node.set(description_attribute{}, "Master gamma (0.1 to 3.0, 1 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::FLOAT) {
                app_->renderer().masterProperties().colorAdjust.gamma = v.get<float>();
            }
        });
        
        parameters_[basePath + "/gamma"] = param;
    }
}

void OSCQueryServer::registerOSDParameters() {
    if (!device_ || !app_) {
        return;
    }
    
    std::string basePath = "/videocomposer/osd";
    
    // OSD frame
    {
        auto& node = findOrCreateNode(basePath + "/frame");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable frame counter display");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, false);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::BOOL) {
                OSDManager* osd = app_->getOSDManager();
                if (osd) {
                    if (v.get<bool>()) {
                        osd->enableMode(OSDManager::FRAME);
                    } else {
                        osd->disableMode(OSDManager::FRAME);
                    }
                }
            }
        });
        
        parameters_[basePath + "/frame"] = param;
    }
    
    // OSD SMPTE
    {
        auto& node = findOrCreateNode(basePath + "/smpte");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable SMPTE timecode display");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, false);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::BOOL) {
                OSDManager* osd = app_->getOSDManager();
                if (osd) {
                    if (v.get<bool>()) {
                        osd->enableMode(OSDManager::SMPTE);
                    } else {
                        osd->disableMode(OSDManager::SMPTE);
                    }
                }
            }
        });
        
        parameters_[basePath + "/smpte"] = param;
    }
    
    // OSD text
    {
        auto& node = findOrCreateNode(basePath + "/text");
        auto param = node.create_parameter(ossia::val_type::STRING);
        node.set(description_attribute{}, "OSD text content");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, std::string(""));
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::STRING) {
                OSDManager* osd = app_->getOSDManager();
                if (osd) {
                    osd->setText(v.get<std::string>());
                }
            }
        });
        
        parameters_[basePath + "/text"] = param;
    }
    
    // OSD box
    {
        auto& node = findOrCreateNode(basePath + "/box");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable OSD background box");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, false);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::BOOL) {
                OSDManager* osd = app_->getOSDManager();
                if (osd) {
                    if (v.get<bool>()) {
                        osd->enableMode(OSDManager::BOX);
                    } else {
                        osd->disableMode(OSDManager::BOX);
                    }
                }
            }
        });
        
        parameters_[basePath + "/box"] = param;
    }
    
    // OSD position (Vec2f)
    {
        auto& node = findOrCreateNode(basePath + "/pos");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        node.set(description_attribute{}, "OSD position (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this](const ossia::value& v) {
            if (app_ && v.get_type() == ossia::val_type::VEC2F) {
                OSDManager* osd = app_->getOSDManager();
                if (osd) {
                    auto vec = v.get<ossia::vec2f>();
                    osd->setTextPosition(static_cast<int>(vec[0]), static_cast<int>(vec[1]));
                }
            }
        });
        
        parameters_[basePath + "/pos"] = param;
    }
}

void OSCQueryServer::registerLayerParameters(int layerId) {
    if (!device_ || !layerManager_) {
        return;
    }
    
    VideoLayer* layer = layerManager_->getLayer(layerId);
    if (!layer) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string basePath = "/videocomposer/layer/" + std::to_string(layerId);
    std::vector<std::string> paths;
    
    // Opacity
    {
        auto& node = findOrCreateNode(basePath + "/opacity");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 1.0f));
        node.set(description_attribute{}, "Layer opacity (0.0 = transparent, 1.0 = opaque)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().opacity = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/opacity";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Visible
    {
        auto& node = findOrCreateNode(basePath + "/visible");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Layer visibility");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, true);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::BOOL) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().visible = v.get<bool>();
                }
            }
        });
        
        std::string path = basePath + "/visible";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Position (Vec2f)
    {
        auto& node = findOrCreateNode(basePath + "/position");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        node.set(description_attribute{}, "Layer position (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::VEC2F) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    auto vec = v.get<ossia::vec2f>();
                    layer->properties().x = static_cast<int>(vec[0]);
                    layer->properties().y = static_cast<int>(vec[1]);
                }
            }
        });
        
        std::string path = basePath + "/position";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Scale (Vec2f)
    {
        auto& node = findOrCreateNode(basePath + "/scale");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        node.set(description_attribute{}, "Layer scale (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, ossia::vec2f{1.0f, 1.0f});
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::VEC2F) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    auto vec = v.get<ossia::vec2f>();
                    layer->properties().scaleX = vec[0];
                    layer->properties().scaleY = vec[1];
                }
            }
        });
        
        std::string path = basePath + "/scale";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // X Scale
    {
        auto& node = findOrCreateNode(basePath + "/xscale");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(description_attribute{}, "Layer X scale");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().scaleX = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/xscale";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Y Scale
    {
        auto& node = findOrCreateNode(basePath + "/yscale");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(description_attribute{}, "Layer Y scale");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().scaleY = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/yscale";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Rotation
    {
        auto& node = findOrCreateNode(basePath + "/rotation");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 360.0f));
        node.set(description_attribute{}, "Layer rotation in degrees (0-360)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().rotation = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/rotation";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Z Order
    {
        auto& node = findOrCreateNode(basePath + "/zorder");
        auto param = node.create_parameter(ossia::val_type::INT);
        node.set(description_attribute{}, "Layer stacking order");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::INT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().zOrder = v.get<int>();
                    if (layerManager_) {
                        layerManager_->setLayerZOrder(layerId, v.get<int>());
                    }
                }
            }
        });
        
        std::string path = basePath + "/zorder";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Blend mode
    {
        auto& node = findOrCreateNode(basePath + "/blendmode");
        auto param = node.create_parameter(ossia::val_type::INT);
        node.set(domain_attribute{}, make_domain(0, 3));
        node.set(description_attribute{}, "Blend mode (0=normal, 1=multiply, 2=screen, 3=overlay)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::INT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    int mode = v.get<int>();
                    if (mode >= 0 && mode <= 3) {
                        layer->properties().blendMode = static_cast<LayerProperties::BlendMode>(mode);
                    }
                }
            }
        });
        
        std::string path = basePath + "/blendmode";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // File path
    {
        auto& node = findOrCreateNode(basePath + "/file");
        auto param = node.create_parameter(ossia::val_type::STRING);
        node.set(description_attribute{}, "Current video file path");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::STRING) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer && app_) {
                    std::string filepath = v.get<std::string>();
                    std::string cueId = layerManager_->getCueIdFromLayer(layer);
                    app_->loadFileIntoLayer(cueId, filepath);
                }
            }
        });
        
        std::string path = basePath + "/file";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // MTC Follow
    {
        auto& node = findOrCreateNode(basePath + "/mtcfollow");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable MTC following for this layer");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, true);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::BOOL) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->setMtcFollow(v.get<bool>());
                }
            }
        });
        
        std::string path = basePath + "/mtcfollow";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Loop
    {
        auto& node = findOrCreateNode(basePath + "/loop");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable looping");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, false);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::BOOL) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->setWraparound(v.get<bool>());
                }
            }
        });
        
        std::string path = basePath + "/loop";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Time scale
    {
        auto& node = findOrCreateNode(basePath + "/timescale");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(description_attribute{}, "Time scale multiplier");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->setTimeScale(static_cast<double>(v.get<float>()));
                }
            }
        });
        
        std::string path = basePath + "/timescale";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Brightness
    {
        auto& node = findOrCreateNode(basePath + "/brightness");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(-1.0f, 1.0f));
        node.set(description_attribute{}, "Brightness adjustment (-1.0 to 1.0, 0 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().colorAdjust.brightness = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/brightness";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Contrast
    {
        auto& node = findOrCreateNode(basePath + "/contrast");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 2.0f));
        node.set(description_attribute{}, "Contrast adjustment (0.0 to 2.0, 1 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().colorAdjust.contrast = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/contrast";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Saturation
    {
        auto& node = findOrCreateNode(basePath + "/saturation");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.0f, 2.0f));
        node.set(description_attribute{}, "Saturation adjustment (0.0 to 2.0, 1 = no change, 0 = grayscale)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().colorAdjust.saturation = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/saturation";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Hue
    {
        auto& node = findOrCreateNode(basePath + "/hue");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(-180.0f, 180.0f));
        node.set(description_attribute{}, "Hue shift in degrees (-180 to 180)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().colorAdjust.hue = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/hue";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Gamma
    {
        auto& node = findOrCreateNode(basePath + "/gamma");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        node.set(domain_attribute{}, make_domain(0.1f, 3.0f));
        node.set(description_attribute{}, "Gamma adjustment (0.1 to 3.0, 1 = no change)");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 1.0f);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::FLOAT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().colorAdjust.gamma = v.get<float>();
                }
            }
        });
        
        std::string path = basePath + "/gamma";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Crop (Vec4f: x, y, width, height)
    {
        auto& node = findOrCreateNode(basePath + "/crop");
        auto param = node.create_parameter(ossia::val_type::VEC4F);
        node.set(description_attribute{}, "Crop rectangle (x, y, width, height)");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::VEC4F) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    auto vec = v.get<ossia::vec4f>();
                    layer->properties().crop.x = static_cast<int>(vec[0]);
                    layer->properties().crop.y = static_cast<int>(vec[1]);
                    layer->properties().crop.width = static_cast<int>(vec[2]);
                    layer->properties().crop.height = static_cast<int>(vec[3]);
                    layer->properties().crop.enabled = true;
                }
            }
        });
        
        std::string path = basePath + "/crop";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Panorama
    {
        auto& node = findOrCreateNode(basePath + "/panorama");
        auto param = node.create_parameter(ossia::val_type::BOOL);
        node.set(description_attribute{}, "Enable/disable panorama mode");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, false);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::BOOL) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().panoramaMode = v.get<bool>();
                }
            }
        });
        
        std::string path = basePath + "/panorama";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Pan offset
    {
        auto& node = findOrCreateNode(basePath + "/pan");
        auto param = node.create_parameter(ossia::val_type::INT);
        node.set(description_attribute{}, "Pan offset for panorama mode");
        node.set(access_mode_attribute{}, access_mode::BI);
        node.set(default_value_attribute{}, 0);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (layerManager_ && v.get_type() == ossia::val_type::INT) {
                VideoLayer* layer = layerManager_->getLayer(layerId);
                if (layer) {
                    layer->properties().panOffset = v.get<int>();
                }
            }
        });
        
        std::string path = basePath + "/pan";
        parameters_[path] = param;
        paths.push_back(path);
    }
    
    // Store paths for this layer
    layerParameterPaths_[layerId] = paths;
}

void OSCQueryServer::unregisterLayerParameters(int layerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = layerParameterPaths_.find(layerId);
    if (it != layerParameterPaths_.end()) {
        // Remove all parameters for this layer
        for (const auto& path : it->second) {
            parameters_.erase(path);
        }
        layerParameterPaths_.erase(it);
        
        // Remove node from device tree
        if (device_) {
            std::string basePath = "/videocomposer/layer/" + std::to_string(layerId);
            auto* node = ossia::net::find_node(*device_, basePath);
            if (node) {
                auto* parent = node->get_parent();
                if (parent) {
                    parent->remove_child(std::to_string(layerId));
                }
            }
        }
    }
}

// Update methods - push values to OSCQuery clients
void OSCQueryServer::updateLayerOpacity(int layerId, float opacity) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/opacity";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(opacity);
    }
}

void OSCQueryServer::updateLayerPosition(int layerId, float x, float y) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/position";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec2f{x, y});
    }
}

void OSCQueryServer::updateLayerScale(int layerId, float scaleX, float scaleY) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/scale";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec2f{scaleX, scaleY});
    }
}

void OSCQueryServer::updateLayerRotation(int layerId, float rotation) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/rotation";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(rotation);
    }
}

void OSCQueryServer::updateLayerVisible(int layerId, bool visible) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/visible";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(visible);
    }
}

void OSCQueryServer::updateLayerZOrder(int layerId, int zOrder) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/zorder";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(zOrder);
    }
}

void OSCQueryServer::updateLayerBlendMode(int layerId, int blendMode) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/blendmode";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(blendMode);
    }
}

void OSCQueryServer::updateLayerFile(int layerId, const std::string& filepath) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/file";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(filepath);
    }
}

void OSCQueryServer::updateLayerMtcFollow(int layerId, bool enabled) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/mtcfollow";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateLayerLoop(int layerId, bool enabled) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/loop";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateLayerTimeScale(int layerId, double timescale) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/timescale";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(static_cast<float>(timescale));
    }
}

void OSCQueryServer::updateLayerColorAdjustment(int layerId, float brightness, float contrast,
                                                 float saturation, float hue, float gamma) {
    std::string basePath = "/videocomposer/layer/" + std::to_string(layerId);
    
    auto updateParam = [this](const std::string& path, float value) {
        auto it = parameters_.find(path);
        if (it != parameters_.end() && it->second) {
            it->second->push_value(value);
        }
    };
    
    updateParam(basePath + "/brightness", brightness);
    updateParam(basePath + "/contrast", contrast);
    updateParam(basePath + "/saturation", saturation);
    updateParam(basePath + "/hue", hue);
    updateParam(basePath + "/gamma", gamma);
}

void OSCQueryServer::updateLayerCrop(int layerId, int x, int y, int width, int height) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/crop";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec4f{static_cast<float>(x), static_cast<float>(y),
                                              static_cast<float>(width), static_cast<float>(height)});
    }
}

void OSCQueryServer::updateLayerPanorama(int layerId, bool enabled) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/panorama";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateLayerPan(int layerId, int panOffset) {
    std::string path = "/videocomposer/layer/" + std::to_string(layerId) + "/pan";
    auto it = parameters_.find(path);
    if (it != parameters_.end() && it->second) {
        it->second->push_value(panOffset);
    }
}

void OSCQueryServer::updateMasterOpacity(float opacity) {
    auto it = parameters_.find("/videocomposer/master/opacity");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(opacity);
    }
}

void OSCQueryServer::updateMasterPosition(float x, float y) {
    auto it = parameters_.find("/videocomposer/master/position");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec2f{x, y});
    }
}

void OSCQueryServer::updateMasterScale(float scaleX, float scaleY) {
    auto it = parameters_.find("/videocomposer/master/scale");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec2f{scaleX, scaleY});
    }
}

void OSCQueryServer::updateMasterRotation(float rotation) {
    auto it = parameters_.find("/videocomposer/master/rotation");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(rotation);
    }
}

void OSCQueryServer::updateMasterColorAdjustment(float brightness, float contrast,
                                                  float saturation, float hue, float gamma) {
    auto updateParam = [this](const std::string& path, float value) {
        auto it = parameters_.find(path);
        if (it != parameters_.end() && it->second) {
            it->second->push_value(value);
        }
    };
    
    updateParam("/videocomposer/master/brightness", brightness);
    updateParam("/videocomposer/master/contrast", contrast);
    updateParam("/videocomposer/master/saturation", saturation);
    updateParam("/videocomposer/master/hue", hue);
    updateParam("/videocomposer/master/gamma", gamma);
}

void OSCQueryServer::updateOSDFrame(bool enabled) {
    auto it = parameters_.find("/videocomposer/osd/frame");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateOSDSMPTE(bool enabled) {
    auto it = parameters_.find("/videocomposer/osd/smpte");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateOSDText(const std::string& text) {
    auto it = parameters_.find("/videocomposer/osd/text");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(text);
    }
}

void OSCQueryServer::updateOSDBox(bool enabled) {
    auto it = parameters_.find("/videocomposer/osd/box");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(enabled);
    }
}

void OSCQueryServer::updateOSDPos(int x, int y) {
    auto it = parameters_.find("/videocomposer/osd/pos");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(ossia::vec2f{static_cast<float>(x), static_cast<float>(y)});
    }
}

void OSCQueryServer::updateFPS(double fps) {
    auto it = parameters_.find("/videocomposer/fps");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(static_cast<float>(fps));
    }
}

void OSCQueryServer::updateOffset(int64_t offset) {
    auto it = parameters_.find("/videocomposer/offset");
    if (it != parameters_.end() && it->second) {
        it->second->push_value(static_cast<int>(offset));
    }
}

void OSCQueryServer::onLayerAdded(int layerId) {
    registerLayerParameters(layerId);
}

void OSCQueryServer::onLayerRemoved(int layerId) {
    unregisterLayerParameters(layerId);
}

} // namespace videocomposer

#endif // HAVE_OSCQUERY

