/**
 * DRMOutputManager.cpp - DRM/KMS output enumeration implementation
 */

#include "DRMOutputManager.h"
#include "../../utils/Logger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

// For EDID parsing
#include <libdrm/drm_mode.h>

namespace videocomposer {

// DRM connector type names
static const char* connectorTypeNames[] = {
    "Unknown",
    "VGA",
    "DVII",
    "DVID",
    "DVIA",
    "Composite",
    "SVIDEO",
    "LVDS",
    "Component",
    "9PinDIN",
    "DisplayPort",
    "HDMIA",
    "HDMIB",
    "TV",
    "eDP",
    "VIRTUAL",
    "DSI",
    "DPI",
    "WRITEBACK",
    "SPI",
    "USB"
};

DRMOutputManager::DRMOutputManager() {
}

DRMOutputManager::~DRMOutputManager() {
    cleanup();
}

bool DRMOutputManager::init(const std::string& devicePath) {
    // Determine device path
    std::string path = devicePath;
    if (path.empty()) {
        path = autoDetectDevice();
        if (path.empty()) {
            LOG_ERROR << "DRMOutputManager: No DRM device found";
            return false;
        }
    }
    
    // Open DRM device
    if (!openDRMDevice(path)) {
        return false;
    }
    
    // Enable atomic modesetting if available
    enableAtomic();
    
    // Get DRM resources
    resources_ = drmModeGetResources(drmFd_);
    if (!resources_) {
        LOG_ERROR << "DRMOutputManager: Failed to get DRM resources";
        
        // Check if this is an NVIDIA card and provide helpful hint
        drmVersion* version = drmGetVersion(drmFd_);
        if (version) {
            std::string driverName(version->name, version->name_len);
            drmFreeVersion(version);
            
            if (driverName == "nvidia-drm" || driverName.find("nvidia") != std::string::npos) {
                LOG_ERROR << "DRMOutputManager: NVIDIA driver detected without KMS support";
                LOG_ERROR << "DRMOutputManager: To fix, enable modesetting:";
                LOG_ERROR << "DRMOutputManager:   1. Add 'nvidia-drm.modeset=1' to kernel parameters, or";
                LOG_ERROR << "DRMOutputManager:   2. Run: echo 'options nvidia-drm modeset=1' | sudo tee /etc/modprobe.d/nvidia-drm.conf";
                LOG_ERROR << "DRMOutputManager:   3. Then: sudo update-initramfs -u && sudo reboot";
            } else {
                LOG_ERROR << "DRMOutputManager: Driver '" << driverName << "' may not support KMS";
                LOG_ERROR << "DRMOutputManager: Ensure no other process (compositor/X11) holds DRM master";
            }
        }
        
        cleanup();
        return false;
    }
    
    LOG_INFO << "DRMOutputManager: Device " << devicePath_ 
             << " has " << resources_->count_connectors << " connectors, "
             << resources_->count_crtcs << " CRTCs";
    
    // Detect outputs
    if (!detectOutputs()) {
        LOG_ERROR << "DRMOutputManager: Failed to detect outputs";
        cleanup();
        return false;
    }
    
    LOG_INFO << "DRMOutputManager: Detected " << outputs_.size() << " connected outputs";
    
    return true;
}

void DRMOutputManager::cleanup() {
    // Restore original modes
    restoreOriginalModes();
    
    // Free connectors
    for (auto& conn : connectors_) {
        if (conn.savedCrtc) {
            drmModeFreeCrtc(conn.savedCrtc);
            conn.savedCrtc = nullptr;
        }
        if (conn.connector) {
            drmModeFreeConnector(conn.connector);
            conn.connector = nullptr;
        }
    }
    connectors_.clear();
    outputs_.clear();
    outputsByName_.clear();
    crtcToConnector_.clear();
    
    // Free resources
    if (resources_) {
        drmModeFreeResources(resources_);
        resources_ = nullptr;
    }
    
    // Close device
    if (drmFd_ >= 0) {
        close(drmFd_);
        drmFd_ = -1;
    }
    
    devicePath_.clear();
}

std::string DRMOutputManager::autoDetectDevice() {
    // Try to find a DRM device with connected outputs
    DIR* dir = opendir("/dev/dri");
    if (!dir) {
        LOG_ERROR << "DRMOutputManager: Cannot open /dev/dri";
        return "";
    }
    
    std::string bestDevice;
    int bestConnected = 0;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Look for card* devices (not renderD*)
        if (strncmp(entry->d_name, "card", 4) != 0) {
            continue;
        }
        
        std::string path = std::string("/dev/dri/") + entry->d_name;
        
        // Try to open device
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        
        // Check for connected outputs
        drmModeRes* res = drmModeGetResources(fd);
        if (res) {
            int connected = 0;
            for (int i = 0; i < res->count_connectors; ++i) {
                drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
                if (conn) {
                    if (conn->connection == DRM_MODE_CONNECTED) {
                        connected++;
                    }
                    drmModeFreeConnector(conn);
                }
            }
            
            if (connected > bestConnected) {
                bestConnected = connected;
                bestDevice = path;
            }
            
            drmModeFreeResources(res);
        }
        
        close(fd);
    }
    
    closedir(dir);
    
    if (bestDevice.empty()) {
        // Fallback to card0
        struct stat st;
        if (stat("/dev/dri/card0", &st) == 0) {
            bestDevice = "/dev/dri/card0";
        }
    }
    
    if (!bestDevice.empty()) {
        LOG_INFO << "DRMOutputManager: Auto-detected device: " << bestDevice;
    }
    
    return bestDevice;
}

bool DRMOutputManager::openDRMDevice(const std::string& path) {
    drmFd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (drmFd_ < 0) {
        LOG_ERROR << "DRMOutputManager: Cannot open " << path 
                  << ": " << strerror(errno);
        LOG_ERROR << "Ensure user is in 'video' group or running as root";
        return false;
    }
    
    // Check if this is a master DRM device
    drm_magic_t magic;
    if (drmGetMagic(drmFd_, &magic) == 0) {
        if (drmAuthMagic(drmFd_, magic) != 0) {
            // Not master, try to become master
            if (drmSetMaster(drmFd_) != 0) {
                LOG_WARNING << "DRMOutputManager: Could not become DRM master";
                // Continue anyway - might work for rendering
            }
        }
    }
    
    devicePath_ = path;
    LOG_INFO << "DRMOutputManager: Opened " << path;
    
    return true;
}

void DRMOutputManager::enableAtomic() {
    // Try to enable atomic modesetting
    if (drmSetClientCap(drmFd_, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
        atomicSupported_ = true;
        LOG_INFO << "DRMOutputManager: Atomic modesetting enabled";
    } else {
        atomicSupported_ = false;
        LOG_INFO << "DRMOutputManager: Atomic modesetting not available";
    }
    
    // Enable universal planes (needed for atomic)
    drmSetClientCap(drmFd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
}

bool DRMOutputManager::detectOutputs() {
    if (!resources_) {
        return false;
    }
    
    connectors_.clear();
    outputs_.clear();
    outputsByName_.clear();
    
    // Iterate through all connectors
    for (int i = 0; i < resources_->count_connectors; ++i) {
        drmModeConnector* conn = drmModeGetConnector(drmFd_, resources_->connectors[i]);
        if (!conn) {
            continue;
        }
        
        DRMConnector drmConn;
        drmConn.connectorId = conn->connector_id;
        drmConn.connector = conn;
        drmConn.info.connected = (conn->connection == DRM_MODE_CONNECTED);
        drmConn.info.index = static_cast<int32_t>(connectors_.size());
        
        // Build connector name
        drmConn.info.name = getConnectorName(conn);
        
        // Only process connected outputs
        if (drmConn.info.connected) {
            // Parse modes
            parseModes(drmConn);
            
            // Read EDID
            readEDID(drmConn);
            
            // Get connector properties
            getConnectorProperties(drmConn);
            
            // Allocate CRTC
            if (allocateCrtc(drmConn)) {
                drmConn.info.enabled = true;
                
                // Save original CRTC state
                drmConn.savedCrtc = drmModeGetCrtc(drmFd_, drmConn.crtcId);
                if (drmConn.savedCrtc && drmConn.savedCrtc->mode_valid) {
                    // Use current mode from CRTC
                    drmConn.info.width = drmConn.savedCrtc->width;
                    drmConn.info.height = drmConn.savedCrtc->height;
                    drmConn.info.x = drmConn.savedCrtc->x;
                    drmConn.info.y = drmConn.savedCrtc->y;
                    
                    // Get refresh rate from current mode
                    drmModeModeInfo* mode = &drmConn.savedCrtc->mode;
                    if (mode->htotal && mode->vtotal) {
                        drmConn.info.refreshRate = static_cast<double>(mode->clock * 1000) /
                                                   (mode->htotal * mode->vtotal);
                    }
                } else {
                    // No current mode set (no compositor running) - use preferred or first available mode
                    drmModeModeInfo* bestMode = nullptr;
                    
                    // Find preferred mode first
                    for (int m = 0; m < conn->count_modes; ++m) {
                        if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                            bestMode = &conn->modes[m];
                            break;
                        }
                    }
                    
                    // Fall back to first mode if no preferred
                    if (!bestMode && conn->count_modes > 0) {
                        bestMode = &conn->modes[0];
                    }
                    
                    if (bestMode) {
                        drmConn.info.width = bestMode->hdisplay;
                        drmConn.info.height = bestMode->vdisplay;
                        drmConn.info.x = 0;
                        drmConn.info.y = 0;
                        
                        if (bestMode->htotal && bestMode->vtotal) {
                            drmConn.info.refreshRate = static_cast<double>(bestMode->clock * 1000) /
                                                       (bestMode->htotal * bestMode->vtotal);
                        }
                        
                        LOG_INFO << "DRMOutputManager: No active mode on " << drmConn.info.name 
                                 << ", will use " << bestMode->hdisplay << "x" << bestMode->vdisplay
                                 << (bestMode->type & DRM_MODE_TYPE_PREFERRED ? " (preferred)" : "");
                    } else {
                        LOG_WARNING << "DRMOutputManager: No modes available for " << drmConn.info.name;
                    }
                }
            }
            
            LOG_INFO << "DRMOutputManager: Found output " << drmConn.info.name
                     << " (" << drmConn.info.getDisplayName() << ")"
                     << " " << drmConn.info.width << "x" << drmConn.info.height
                     << "@" << drmConn.info.refreshRate << "Hz";
            
            // Add to outputs list
            outputs_.push_back(drmConn.info);
            outputsByName_[drmConn.info.name] = outputs_.size() - 1;
        } else {
            // Disconnected connector - still track it
            drmConn.info.enabled = false;
        }
        
        connectors_.push_back(std::move(drmConn));
    }
    
    return !outputs_.empty();
}

std::string DRMOutputManager::getConnectorTypeName(uint32_t type) const {
    if (type < sizeof(connectorTypeNames) / sizeof(connectorTypeNames[0])) {
        return connectorTypeNames[type];
    }
    return "Unknown";
}

std::string DRMOutputManager::getConnectorName(const drmModeConnector* conn) const {
    std::string typeName;
    
    switch (conn->connector_type) {
        case DRM_MODE_CONNECTOR_VGA:
            typeName = "VGA";
            break;
        case DRM_MODE_CONNECTOR_DVII:
        case DRM_MODE_CONNECTOR_DVID:
        case DRM_MODE_CONNECTOR_DVIA:
            typeName = "DVI";
            break;
        case DRM_MODE_CONNECTOR_HDMIA:
        case DRM_MODE_CONNECTOR_HDMIB:
            typeName = "HDMI-A";
            break;
        case DRM_MODE_CONNECTOR_DisplayPort:
            typeName = "DP";
            break;
        case DRM_MODE_CONNECTOR_eDP:
            typeName = "eDP";
            break;
        case DRM_MODE_CONNECTOR_LVDS:
            typeName = "LVDS";
            break;
        case DRM_MODE_CONNECTOR_DSI:
            typeName = "DSI";
            break;
        default:
            typeName = getConnectorTypeName(conn->connector_type);
            break;
    }
    
    return typeName + "-" + std::to_string(conn->connector_type_id);
}

void DRMOutputManager::parseModes(DRMConnector& connector) {
    if (!connector.connector) {
        return;
    }
    
    connector.info.modes.clear();
    
    for (int i = 0; i < connector.connector->count_modes; ++i) {
        const drmModeModeInfo& mode = connector.connector->modes[i];
        
        OutputMode outMode;
        outMode.width = mode.hdisplay;
        outMode.height = mode.vdisplay;
        
        // Calculate refresh rate
        if (mode.htotal && mode.vtotal) {
            outMode.refreshRate = static_cast<double>(mode.clock * 1000) /
                                  (mode.htotal * mode.vtotal);
        }
        
        outMode.preferred = (mode.type & DRM_MODE_TYPE_PREFERRED) != 0;
        
        connector.info.modes.push_back(outMode);
    }
}

void DRMOutputManager::readEDID(DRMConnector& connector) {
    if (!connector.connector) {
        return;
    }
    
    // Find EDID property
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        drmFd_, connector.connectorId, DRM_MODE_OBJECT_CONNECTOR);
    
    if (!props) {
        return;
    }
    
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);
        if (!prop) {
            continue;
        }
        
        if (strcmp(prop->name, "EDID") == 0 && props->prop_values[i]) {
            drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(
                drmFd_, static_cast<uint32_t>(props->prop_values[i]));
            
            if (blob && blob->length >= 128) {
                const uint8_t* edid = static_cast<const uint8_t*>(blob->data);
                
                // Parse EDID header (bytes 0-7 should be 00 FF FF FF FF FF FF 00)
                if (edid[0] == 0x00 && edid[1] == 0xFF) {
                    // Manufacturer ID (bytes 8-9)
                    uint16_t mfgId = (edid[8] << 8) | edid[9];
                    char mfg[4];
                    mfg[0] = ((mfgId >> 10) & 0x1F) + 'A' - 1;
                    mfg[1] = ((mfgId >> 5) & 0x1F) + 'A' - 1;
                    mfg[2] = (mfgId & 0x1F) + 'A' - 1;
                    mfg[3] = '\0';
                    connector.info.make = mfg;
                    
                    // Physical size (bytes 21-22) - in cm, convert to mm
                    connector.info.physicalWidthMM = edid[21] * 10;
                    connector.info.physicalHeightMM = edid[22] * 10;
                    
                    // Look for descriptor blocks for monitor name
                    for (int desc = 0; desc < 4; ++desc) {
                        int offset = 54 + desc * 18;
                        if (offset + 18 > static_cast<int>(blob->length)) break;
                        
                        // Monitor name descriptor: 00 00 00 FC 00
                        if (edid[offset] == 0x00 && edid[offset + 1] == 0x00 &&
                            edid[offset + 2] == 0x00 && edid[offset + 3] == 0xFC) {
                            
                            char name[14];
                            memcpy(name, &edid[offset + 5], 13);
                            name[13] = '\0';
                            
                            // Remove trailing whitespace/newlines
                            for (int j = 12; j >= 0; --j) {
                                if (name[j] == ' ' || name[j] == '\n' || name[j] == '\r') {
                                    name[j] = '\0';
                                } else {
                                    break;
                                }
                            }
                            
                            connector.info.model = name;
                        }
                        
                        // Serial number descriptor: 00 00 00 FF 00
                        if (edid[offset] == 0x00 && edid[offset + 1] == 0x00 &&
                            edid[offset + 2] == 0x00 && edid[offset + 3] == 0xFF) {
                            
                            char serial[14];
                            memcpy(serial, &edid[offset + 5], 13);
                            serial[13] = '\0';
                            
                            for (int j = 12; j >= 0; --j) {
                                if (serial[j] == ' ' || serial[j] == '\n' || serial[j] == '\r') {
                                    serial[j] = '\0';
                                } else {
                                    break;
                                }
                            }
                            
                            connector.info.serialNumber = serial;
                        }
                    }
                }
                
                drmModeFreePropertyBlob(blob);
            }
        }
        
        drmModeFreeProperty(prop);
    }
    
    drmModeFreeObjectProperties(props);
}

void DRMOutputManager::getConnectorProperties(DRMConnector& connector) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        drmFd_, connector.connectorId, DRM_MODE_OBJECT_CONNECTOR);
    
    if (!props) {
        return;
    }
    
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);
        if (!prop) {
            continue;
        }
        
        if (strcmp(prop->name, "CRTC_ID") == 0) {
            connector.propCrtcId = prop->prop_id;
        } else if (strcmp(prop->name, "DPMS") == 0) {
            connector.propDpms = prop->prop_id;
        }
        
        drmModeFreeProperty(prop);
    }
    
    drmModeFreeObjectProperties(props);
}

bool DRMOutputManager::allocateCrtc(DRMConnector& connector) {
    if (!connector.connector || !resources_) {
        return false;
    }
    
    // First, try the currently connected encoder's CRTC
    if (connector.connector->encoder_id) {
        drmModeEncoder* encoder = drmModeGetEncoder(drmFd_, connector.connector->encoder_id);
        if (encoder) {
            if (encoder->crtc_id && isCrtcAvailable(encoder->crtc_id)) {
                connector.encoderId = encoder->encoder_id;
                connector.crtcId = encoder->crtc_id;
                crtcToConnector_[connector.crtcId] = connector.connectorId;
                drmModeFreeEncoder(encoder);
                return true;
            }
            drmModeFreeEncoder(encoder);
        }
    }
    
    // Try all encoders for this connector
    for (int i = 0; i < connector.connector->count_encoders; ++i) {
        drmModeEncoder* encoder = drmModeGetEncoder(
            drmFd_, connector.connector->encoders[i]);
        
        if (!encoder) {
            continue;
        }
        
        // Try each CRTC this encoder can use
        for (int j = 0; j < resources_->count_crtcs; ++j) {
            if (!(encoder->possible_crtcs & (1 << j))) {
                continue;
            }
            
            uint32_t crtcId = resources_->crtcs[j];
            if (isCrtcAvailable(crtcId)) {
                connector.encoderId = encoder->encoder_id;
                connector.crtcId = crtcId;
                crtcToConnector_[crtcId] = connector.connectorId;
                drmModeFreeEncoder(encoder);
                return true;
            }
        }
        
        drmModeFreeEncoder(encoder);
    }
    
    LOG_WARNING << "DRMOutputManager: No available CRTC for " << connector.info.name;
    return false;
}

bool DRMOutputManager::isCrtcAvailable(uint32_t crtcId) const {
    return crtcToConnector_.find(crtcId) == crtcToConnector_.end();
}

const DRMConnector* DRMOutputManager::getConnector(int index) const {
    if (index < 0 || index >= static_cast<int>(connectors_.size())) {
        return nullptr;
    }
    return &connectors_[index];
}

const DRMConnector* DRMOutputManager::getConnectorByName(const std::string& name) const {
    for (const auto& conn : connectors_) {
        if (conn.info.name == name) {
            return &conn;
        }
    }
    return nullptr;
}

DRMConnector* DRMOutputManager::getConnector(int index) {
    if (index < 0 || index >= static_cast<int>(connectors_.size())) {
        return nullptr;
    }
    return &connectors_[index];
}

DRMConnector* DRMOutputManager::getConnectorByName(const std::string& name) {
    for (auto& conn : connectors_) {
        if (conn.info.name == name) {
            return &conn;
        }
    }
    return nullptr;
}

bool DRMOutputManager::refreshOutputs() {
    // Re-probe connectors for hotplug changes
    bool changed = false;
    
    for (auto& conn : connectors_) {
        if (!conn.connector) {
            continue;
        }
        
        // Get fresh connector state
        drmModeConnector* fresh = drmModeGetConnector(drmFd_, conn.connectorId);
        if (!fresh) {
            continue;
        }
        
        bool wasConnected = conn.info.connected;
        bool isConnected = (fresh->connection == DRM_MODE_CONNECTED);
        
        if (wasConnected != isConnected) {
            changed = true;
            conn.info.connected = isConnected;
            
            if (isConnected) {
                // Update connector info
                drmModeFreeConnector(conn.connector);
                conn.connector = fresh;
                parseModes(conn);
                readEDID(conn);
                
                if (hotplugCallback_) {
                    hotplugCallback_(conn.info, true);
                }
            } else {
                drmModeFreeConnector(fresh);
                
                // Release CRTC
                auto it = crtcToConnector_.find(conn.crtcId);
                if (it != crtcToConnector_.end()) {
                    crtcToConnector_.erase(it);
                }
                conn.crtcId = 0;
                conn.info.enabled = false;
                
                if (hotplugCallback_) {
                    hotplugCallback_(conn.info, false);
                }
            }
        } else {
            drmModeFreeConnector(fresh);
        }
    }
    
    if (changed) {
        // Rebuild outputs list
        outputs_.clear();
        outputsByName_.clear();
        
        for (const auto& conn : connectors_) {
            if (conn.info.connected && conn.info.enabled) {
                outputs_.push_back(conn.info);
                outputsByName_[conn.info.name] = outputs_.size() - 1;
            }
        }
    }
    
    return changed;
}

drmModeModeInfo* DRMOutputManager::findMode(drmModeConnector* conn, 
                                            int width, int height, 
                                            double refreshRate) {
    if (!conn) {
        return nullptr;
    }
    
    drmModeModeInfo* best = nullptr;
    double bestRefreshDiff = 999.0;
    
    for (int i = 0; i < conn->count_modes; ++i) {
        drmModeModeInfo* mode = &conn->modes[i];
        
        if (mode->hdisplay != static_cast<uint16_t>(width) ||
            mode->vdisplay != static_cast<uint16_t>(height)) {
            continue;
        }
        
        double modeRefresh = 0.0;
        if (mode->htotal && mode->vtotal) {
            modeRefresh = static_cast<double>(mode->clock * 1000) /
                         (mode->htotal * mode->vtotal);
        }
        
        if (refreshRate <= 0.0) {
            // Prefer preferred mode, then highest refresh
            if (!best || (mode->type & DRM_MODE_TYPE_PREFERRED) ||
                modeRefresh > bestRefreshDiff) {
                best = mode;
                bestRefreshDiff = modeRefresh;
            }
        } else {
            double diff = std::abs(modeRefresh - refreshRate);
            if (diff < bestRefreshDiff) {
                best = mode;
                bestRefreshDiff = diff;
            }
        }
    }
    
    return best;
}

bool DRMOutputManager::setMode(int index, int width, int height, double refreshRate) {
    DRMConnector* conn = getConnector(index);
    if (!conn || !conn->connector || !conn->crtcId) {
        LOG_ERROR << "DRMOutputManager: Invalid output index " << index;
        return false;
    }
    
    drmModeModeInfo* mode = findMode(conn->connector, width, height, refreshRate);
    if (!mode) {
        LOG_ERROR << "DRMOutputManager: Mode not found: " 
                  << width << "x" << height << "@" << refreshRate;
        return false;
    }
    
    // Set mode
    int ret = drmModeSetCrtc(drmFd_, conn->crtcId, 0, 0, 0,
                             &conn->connectorId, 1, mode);
    
    if (ret != 0) {
        LOG_ERROR << "DRMOutputManager: drmModeSetCrtc failed: " << strerror(-ret);
        return false;
    }
    
    // Update stored info
    conn->info.width = mode->hdisplay;
    conn->info.height = mode->vdisplay;
    if (mode->htotal && mode->vtotal) {
        conn->info.refreshRate = static_cast<double>(mode->clock * 1000) /
                                 (mode->htotal * mode->vtotal);
    }
    
    LOG_INFO << "DRMOutputManager: Set mode " << conn->info.name << " to "
             << conn->info.width << "x" << conn->info.height 
             << "@" << conn->info.refreshRate << "Hz";
    
    return true;
}

bool DRMOutputManager::setMode(const std::string& name, int width, int height, double refreshRate) {
    DRMConnector* conn = getConnectorByName(name);
    if (!conn) {
        LOG_ERROR << "DRMOutputManager: Output not found: " << name;
        return false;
    }
    
    return setMode(conn->info.index, width, height, refreshRate);
}

void DRMOutputManager::restoreOriginalModes() {
    for (auto& conn : connectors_) {
        if (conn.savedCrtc && conn.crtcId) {
            drmModeSetCrtc(drmFd_,
                          conn.savedCrtc->crtc_id,
                          conn.savedCrtc->buffer_id,
                          conn.savedCrtc->x, conn.savedCrtc->y,
                          &conn.connectorId, 1,
                          &conn.savedCrtc->mode);
        }
    }
}

uint32_t DRMOutputManager::getCrtcId(int index) const {
    const DRMConnector* conn = getConnector(index);
    return conn ? conn->crtcId : 0;
}

uint32_t DRMOutputManager::findCrtcForConnector(const drmModeConnector* connector) {
    if (!connector || !resources_) {
        return 0;
    }
    
    for (int i = 0; i < connector->count_encoders; ++i) {
        drmModeEncoder* encoder = drmModeGetEncoder(drmFd_, connector->encoders[i]);
        if (!encoder) {
            continue;
        }
        
        for (int j = 0; j < resources_->count_crtcs; ++j) {
            if (!(encoder->possible_crtcs & (1 << j))) {
                continue;
            }
            
            uint32_t crtcId = resources_->crtcs[j];
            if (isCrtcAvailable(crtcId)) {
                drmModeFreeEncoder(encoder);
                return crtcId;
            }
        }
        
        drmModeFreeEncoder(encoder);
    }
    
    return 0;
}

drmModeAtomicReq* DRMOutputManager::createAtomicRequest() {
    if (!atomicSupported_) {
        return nullptr;
    }
    return drmModeAtomicAlloc();
}

bool DRMOutputManager::commitAtomic(drmModeAtomicReq* request, uint32_t flags) {
    if (!request || !atomicSupported_) {
        return false;
    }
    
    int ret = drmModeAtomicCommit(drmFd_, request, flags, nullptr);
    if (ret != 0) {
        LOG_ERROR << "DRMOutputManager: Atomic commit failed: " << strerror(-ret);
        return false;
    }
    
    return true;
}

uint32_t DRMOutputManager::getPropertyId(uint32_t objectId, uint32_t objectType,
                                         const std::string& name) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        drmFd_, objectId, objectType);
    
    if (!props) {
        return 0;
    }
    
    uint32_t propId = 0;
    
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);
        if (prop) {
            if (name == prop->name) {
                propId = prop->prop_id;
                drmModeFreeProperty(prop);
                break;
            }
            drmModeFreeProperty(prop);
        }
    }
    
    drmModeFreeObjectProperties(props);
    return propId;
}

uint64_t DRMOutputManager::getPropertyValue(uint32_t objectId, uint32_t objectType,
                                            const std::string& name) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(
        drmFd_, objectId, objectType);
    
    if (!props) {
        return 0;
    }
    
    uint64_t value = 0;
    
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmFd_, props->props[i]);
        if (prop) {
            if (name == prop->name) {
                value = props->prop_values[i];
                drmModeFreeProperty(prop);
                break;
            }
            drmModeFreeProperty(prop);
        }
    }
    
    drmModeFreeObjectProperties(props);
    return value;
}

void DRMOutputManager::pollHotplug() {
    refreshOutputs();
}

} // namespace videocomposer

