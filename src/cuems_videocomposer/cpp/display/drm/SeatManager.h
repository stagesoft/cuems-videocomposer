/**
 * SeatManager.h - libseat wrapper for DRM device access
 * 
 * Provides user-space DRM master access via libseat/logind.
 * This allows DRM/KMS operations without running as root.
 */

#ifndef VIDEOCOMPOSER_SEATMANAGER_H
#define VIDEOCOMPOSER_SEATMANAGER_H

#include <string>
#include <functional>
#include <map>

#ifdef HAVE_LIBSEAT
extern "C" {
#include <libseat.h>
}
#endif

namespace videocomposer {

/**
 * SeatManager - Manages seat/device access via libseat
 * 
 * Handles:
 * - Opening DRM devices with proper permissions
 * - Acquiring/releasing DRM master
 * - Device hotplug notifications
 * 
 * Falls back gracefully if libseat is not available.
 */
class SeatManager {
public:
    SeatManager();
    ~SeatManager();
    
    // Disable copy
    SeatManager(const SeatManager&) = delete;
    SeatManager& operator=(const SeatManager&) = delete;
    
    /**
     * Initialize seat manager
     * @return true on success
     */
    bool init();
    
    /**
     * Cleanup and release all resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * Open a DRM device with proper permissions
     * @param path Device path (e.g., "/dev/dri/card0")
     * @return File descriptor, or -1 on error
     */
    int openDevice(const std::string& path);
    
    /**
     * Close a device opened via openDevice()
     * @param fd File descriptor to close
     */
    void closeDevice(int fd);
    
    /**
     * Enable a device (acquire DRM master)
     * @param fd Device file descriptor
     * @return true on success
     */
    bool enableDevice(int fd);
    
    /**
     * Disable a device (release DRM master)
     * @param fd Device file descriptor
     */
    void disableDevice(int fd);
    
    /**
     * Check if libseat is available
     */
    static bool isAvailable();
    
private:
    bool initialized_ = false;
    
#ifdef HAVE_LIBSEAT
    struct libseat* seat_ = nullptr;
    
    // Device tracking (device_id -> fd mapping)
    struct DeviceInfo {
        int fd = -1;
        int deviceId = -1;
    };
    std::map<int, DeviceInfo> devices_;  // fd -> DeviceInfo
    
    // libseat callbacks
    static void onEnableDevice(struct libseat* seat, void* data);
    static void onDisableDevice(struct libseat* seat, void* data);
#endif
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_SEATMANAGER_H

