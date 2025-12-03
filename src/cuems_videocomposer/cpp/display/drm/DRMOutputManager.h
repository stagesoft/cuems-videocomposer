/**
 * DRMOutputManager.h - DRM/KMS output enumeration and management
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Provides low-level access to display outputs via Linux DRM/KMS API.
 * 
 * Features:
 * - Enumerate all connected displays
 * - Parse EDID for monitor identification
 * - Mode setting (resolution/refresh rate)
 * - Atomic modesetting support
 * - CRTC allocation for multi-output
 */

#ifndef VIDEOCOMPOSER_DRMOUTPUTMANAGER_H
#define VIDEOCOMPOSER_DRMOUTPUTMANAGER_H

#include "../OutputInfo.h"
#include "SeatManager.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <vector>
#include <map>
#include <string>
#include <functional>
#include <memory>

namespace videocomposer {

/**
 * ResolutionMode - How to select display resolution
 */
enum class ResolutionMode {
    NATIVE,         // Use EDID preferred mode (panel's true pixels)
    MAXIMUM,        // Use highest available resolution
    HD_1080P,       // Force 1920x1080 (or closest)
    HD_720P,        // Force 1280x720 (or closest)
    UHD_4K,         // Force 3840x2160 (or closest)
    CUSTOM          // Use custom per-output settings
};

/**
 * DRMConnector - Internal representation of a DRM connector
 */
struct DRMConnector {
    uint32_t connectorId = 0;     // DRM connector ID
    uint32_t encoderId = 0;        // Associated encoder ID
    uint32_t crtcId = 0;           // Associated CRTC ID
    drmModeConnector* connector = nullptr;  // DRM connector (owned, must free)
    drmModeCrtc* savedCrtc = nullptr;       // Original mode for restore
    drmModeModeInfo currentMode = {};       // Current active mode (updated on mode change)
    bool hasCurrentMode = false;            // True if currentMode is valid
    OutputInfo info;               // Parsed output information
    bool acquired = false;         // Have we acquired this connector?
    
    // DRM property IDs (for atomic modesetting)
    uint32_t propCrtcId = 0;
    uint32_t propDpms = 0;
};

/**
 * DRMOutputManager - Manages DRM device and output enumeration
 * 
 * Handles:
 * - Opening DRM device (GPU)
 * - Enumerating connectors and CRTCs
 * - Reading EDID data
 * - Mode setting operations
 * - Atomic commit support
 */
class DRMOutputManager {
public:
    DRMOutputManager();
    ~DRMOutputManager();
    
    // ===== Initialization =====
    
    /**
     * Initialize with DRM device
     * @param devicePath Path to DRM device (e.g., "/dev/dri/card0")
     *                   Empty string = auto-detect
     * @return true on success
     */
    bool init(const std::string& devicePath = "");
    
    /**
     * Cleanup and release DRM resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return drmFd_ >= 0; }
    
    // ===== DRM Device Access =====
    
    /**
     * Get DRM file descriptor (for GBM, EGL, etc.)
     */
    int getFd() const { return drmFd_; }
    
    /**
     * Get device path
     */
    const std::string& getDevicePath() const { return devicePath_; }
    
    // ===== Output Enumeration =====
    
    /**
     * Get all connected outputs (builds list from connectors)
     */
    std::vector<OutputInfo> getOutputs() const;
    
    /**
     * Get connected output count
     */
    size_t getOutputCount() const;
    
    /**
     * Get output info by name (direct access to connector info)
     */
    const OutputInfo* getOutputByName(const std::string& name) const;
    OutputInfo* getOutputByName(const std::string& name);
    
    /**
     * Get DRM connector by index
     */
    const DRMConnector* getConnector(int index) const;
    
    /**
     * Get DRM connector by name
     */
    const DRMConnector* getConnectorByName(const std::string& name) const;
    
    /**
     * Get DRM connector (mutable)
     */
    DRMConnector* getConnector(int index);
    DRMConnector* getConnectorByName(const std::string& name);
    
    /**
     * Refresh output detection (hotplug handling)
     */
    bool refreshOutputs();
    
    // ===== Mode Setting =====
    
    /**
     * Set display mode for an output
     * @param index Output index
     * @param width Desired width
     * @param height Desired height
     * @param refreshRate Desired refresh rate (0 = any)
     * @return true on success
     */
    bool setMode(int index, int width, int height, double refreshRate = 0.0);
    
    /**
     * Set display mode by output name
     */
    bool setMode(const std::string& name, int width, int height, double refreshRate = 0.0);
    
    /**
     * Prepare mode change (store mode without calling drmModeSetCrtc)
     * The actual modeset happens when schedulePageFlip renders with modeSet_=false
     * @return true if mode is valid and available
     */
    bool prepareMode(int index, int width, int height, double refreshRate = 0.0);
    
    // ===== Resolution Mode Selection =====
    
    /**
     * Set global resolution mode policy
     * @param mode Resolution selection mode
     */
    void setResolutionMode(ResolutionMode mode);
    
    /**
     * Get current resolution mode
     */
    ResolutionMode getResolutionMode() const { return resolutionMode_; }
    
    /**
     * Apply resolution mode to all outputs
     * Call after init() to apply the configured resolution policy.
     * @return true if all outputs were configured successfully
     */
    bool applyResolutionMode();
    
    /**
     * Apply resolution mode to a specific output
     * @param index Output index
     * @return true on success
     */
    bool applyResolutionModeToOutput(int index);
    
    /**
     * Find best mode matching resolution criteria
     * @param connector The connector to search
     * @param targetWidth Target width (0 = any)
     * @param targetHeight Target height (0 = any)
     * @param preferHighest If true, prefer highest resolution when multiple match
     * @return Pointer to best mode, or nullptr if none found
     */
    const drmModeModeInfo* findBestMode(const DRMConnector& connector,
                                        int targetWidth = 0, int targetHeight = 0,
                                        bool preferHighest = false) const;
    
    /**
     * Restore original mode for all outputs
     */
    void restoreOriginalModes();
    
    // ===== Atomic Modesetting =====
    
    /**
     * Check if atomic modesetting is supported
     */
    bool supportsAtomic() const { return atomicSupported_; }
    
    /**
     * Create an atomic request
     * @return Atomic request pointer (caller must free)
     */
    drmModeAtomicReq* createAtomicRequest();
    
    /**
     * Commit an atomic request
     * @param request Atomic request to commit
     * @param flags Commit flags (e.g., DRM_MODE_ATOMIC_NONBLOCK)
     * @return true on success
     */
    bool commitAtomic(drmModeAtomicReq* request, uint32_t flags);
    
    // ===== CRTC Management =====
    
    /**
     * Get CRTC ID for an output
     */
    uint32_t getCrtcId(int index) const;
    
    /**
     * Find a free CRTC for a connector
     */
    uint32_t findCrtcForConnector(const drmModeConnector* connector);
    
    // ===== Property Access =====
    
    /**
     * Get property ID by name
     * @param objectId DRM object ID (connector, CRTC, etc.)
     * @param objectType Object type (DRM_MODE_OBJECT_*)
     * @param name Property name
     * @return Property ID, or 0 if not found
     */
    uint32_t getPropertyId(uint32_t objectId, uint32_t objectType, 
                           const std::string& name);
    
    /**
     * Get property value
     */
    uint64_t getPropertyValue(uint32_t objectId, uint32_t objectType,
                              const std::string& name);
    
    // ===== Hotplug Notification =====
    
    using HotplugCallback = std::function<void(const OutputInfo&, bool connected)>;
    
    /**
     * Set hotplug callback
     */
    void setHotplugCallback(HotplugCallback callback) { hotplugCallback_ = std::move(callback); }
    
    /**
     * Poll for hotplug events
     * Call periodically or when udev events indicate DRM changes
     */
    void pollHotplug();
    
private:
    // Seat management (for user-space DRM master)
    std::unique_ptr<SeatManager> seatManager_;
    
    // DRM device
    int drmFd_ = -1;
    std::string devicePath_;
    bool atomicSupported_ = false;
    ResolutionMode resolutionMode_ = ResolutionMode::HD_1080P;  // Default to 1080p
    
    // DRM resources
    drmModeRes* resources_ = nullptr;
    
    // Connectors (single source of truth for output info)
    std::vector<DRMConnector> connectors_;
    
    // CRTC allocation tracking
    std::map<uint32_t, uint32_t> crtcToConnector_;  // crtcId -> connectorId
    
    // Hotplug
    HotplugCallback hotplugCallback_;
    
    // ===== Private Methods =====
    
    /**
     * Open DRM device
     */
    bool openDRMDevice(const std::string& path);
    
    /**
     * Auto-detect DRM device
     */
    std::string autoDetectDevice();
    
    /**
     * Detect all outputs
     */
    bool detectOutputs();
    
    /**
     * Get connector type name (HDMI, DP, VGA, etc.)
     */
    std::string getConnectorTypeName(uint32_t type) const;
    
    /**
     * Build connector name (e.g., "HDMI-A-1")
     */
    std::string getConnectorName(const drmModeConnector* conn) const;
    
    /**
     * Read and parse EDID data
     */
    void readEDID(DRMConnector& connector);
    
    /**
     * Parse modes from connector
     */
    void parseModes(DRMConnector& connector);
    
    /**
     * Find matching mode
     */
    drmModeModeInfo* findMode(drmModeConnector* conn, int width, int height, 
                               double refreshRate);
    
    /**
     * Get connector properties
     */
    void getConnectorProperties(DRMConnector& connector);
    
    /**
     * Allocate CRTC for connector
     */
    bool allocateCrtc(DRMConnector& connector);
    
    /**
     * Check if CRTC is available
     */
    bool isCrtcAvailable(uint32_t crtcId) const;
    
    /**
     * Enable atomic modesetting if supported
     */
    void enableAtomic();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMOUTPUTMANAGER_H

