/**
 * SeatManager.cpp - libseat wrapper implementation
 */

#include "SeatManager.h"
#include "../../utils/Logger.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <map>
#include <errno.h>

#ifdef HAVE_LIBSEAT
#include <libseat.h>
#endif

#ifndef HAVE_LIBSEAT
// Fallback: use libdrm directly
#include <xf86drm.h>
#endif

namespace videocomposer {

SeatManager::SeatManager() {
}

SeatManager::~SeatManager() {
    cleanup();
}

bool SeatManager::init() {
    if (initialized_) {
        return true;
    }
    
#ifdef HAVE_LIBSEAT
    seatEnabled_ = false;
    
    // Open seat with callbacks
    struct libseat_seat_listener listener = {
        .enable_seat = onEnableDevice,
        .disable_seat = onDisableDevice,
    };
    
    seat_ = libseat_open_seat(&listener, this);
    if (!seat_) {
        LOG_WARNING << "SeatManager: Failed to open libseat";
        return false;
    }
    
    // Dispatch events until seat is enabled (with timeout to avoid infinite wait)
    int dispatchCount = 0;
    const int maxDispatches = 100;  // Safety limit
    while (!seatEnabled_ && dispatchCount < maxDispatches) {
        if (libseat_dispatch(seat_, 100) < 0) {  // 100ms timeout
            break;
        }
        dispatchCount++;
    }
    
    if (!seatEnabled_) {
        LOG_WARNING << "SeatManager: Seat not enabled after initialization";
        LOG_WARNING << "SeatManager: This may happen if another session is active";
        libseat_close_seat(seat_);
        seat_ = nullptr;
        return false;
    }
    
    initialized_ = true;
    LOG_INFO << "SeatManager: Initialized (libseat, seat enabled)";
    return true;
#else
    // No libseat - will use direct open() which requires root or proper permissions
    initialized_ = true;
    LOG_INFO << "SeatManager: Initialized (fallback mode - requires root or logind session)";
    return true;
#endif
}

void SeatManager::cleanup() {
    if (!initialized_) {
        return;
    }
    
#ifdef HAVE_LIBSEAT
    // Close all devices first
    for (auto& [fd, info] : devices_) {
        closeDevice(fd);
    }
    devices_.clear();
    
    // Disable seat (releases DRM master for all devices)
    if (seat_ && seatEnabled_) {
        libseat_disable_seat(seat_);
        seatEnabled_ = false;
    }
    
    if (seat_) {
        libseat_close_seat(seat_);
        seat_ = nullptr;
    }
#endif
    
    initialized_ = false;
}

int SeatManager::openDevice(const std::string& path) {
    if (!initialized_) {
        LOG_ERROR << "SeatManager: Not initialized";
        return -1;
    }
    
#ifdef HAVE_LIBSEAT
    if (!seat_) {
        LOG_ERROR << "SeatManager: libseat not available";
        return -1;
    }
    
    // Ensure seat is enabled before opening devices
    if (!seatEnabled_) {
        LOG_ERROR << "SeatManager: Seat not enabled, cannot open device";
        LOG_ERROR << "SeatManager: Try dispatching events or check if another session is active";
        return -1;
    }
    
    // Open device via libseat (fd is returned via pointer, device_id is return value)
    int fd = -1;
    int deviceId = libseat_open_device(seat_, path.c_str(), &fd);
    if (deviceId < 0 || fd < 0) {
        LOG_ERROR << "SeatManager: Failed to open device " << path 
                  << ": " << strerror(errno);
        return -1;
    }
    
    // Track device
    DeviceInfo info;
    info.fd = fd;
    info.deviceId = deviceId;
    devices_[fd] = info;
    
    LOG_INFO << "SeatManager: Opened device " << path << " (fd=" << fd << ", device_id=" << deviceId << ")";
    return fd;
#else
    // Fallback: direct open (requires root or proper permissions)
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR << "SeatManager: Failed to open device " << path 
                  << ": " << strerror(errno);
        LOG_ERROR << "SeatManager: Ensure user is in 'video' group or running as root";
        return -1;
    }
    
    LOG_INFO << "SeatManager: Opened device " << path << " (fd=" << fd << ", fallback mode)";
    return fd;
#endif
}

void SeatManager::closeDevice(int fd) {
    if (fd < 0) {
        return;
    }
    
#ifdef HAVE_LIBSEAT
    if (!seat_) {
        close(fd);
        return;
    }
    
    auto it = devices_.find(fd);
    if (it != devices_.end()) {
        // Close device via libseat (using device_id)
        libseat_close_device(seat_, it->second.deviceId);
        devices_.erase(it);
    } else {
        // Not tracked, just close
        close(fd);
    }
#else
    close(fd);
#endif
}

bool SeatManager::enableDevice(int fd) {
    if (fd < 0) {
        return false;
    }
    
#ifdef HAVE_LIBSEAT
    if (!seat_) {
        return false;
    }
    
    // With libseat, devices opened via libseat_open_device() automatically
    // have the right permissions. The seat itself is enabled when opened.
    // We just need to verify the device is tracked.
    auto it = devices_.find(fd);
    if (it == devices_.end()) {
        LOG_ERROR << "SeatManager: Device fd=" << fd << " not tracked";
        return false;
    }
    
    LOG_INFO << "SeatManager: Device enabled (fd=" << fd << ", device_id=" << it->second.deviceId << ")";
    return true;
#else
    // Fallback: try drmSetMaster()
    if (drmSetMaster(fd) != 0) {
        LOG_WARNING << "SeatManager: Failed to acquire DRM master (fd=" << fd 
                    << "): " << strerror(errno);
        LOG_WARNING << "SeatManager: This may fail if another process holds DRM master";
        return false;
    }
    
    LOG_INFO << "SeatManager: Acquired DRM master (fd=" << fd << ", fallback mode)";
    return true;
#endif
}

void SeatManager::disableDevice(int fd) {
    if (fd < 0) {
        return;
    }
    
#ifdef HAVE_LIBSEAT
    if (!seat_) {
        return;
    }
    
    // With libseat, we disable the entire seat (not individual devices)
    // This releases DRM master for all devices
    libseat_disable_seat(seat_);
    LOG_INFO << "SeatManager: Disabled seat (fd=" << fd << ")";
#else
    // Fallback: try drmDropMaster()
    if (drmDropMaster(fd) != 0) {
        LOG_WARNING << "SeatManager: Failed to release DRM master (fd=" << fd 
                    << "): " << strerror(errno);
    } else {
        LOG_INFO << "SeatManager: Released DRM master (fd=" << fd << ", fallback mode)";
    }
#endif
}

bool SeatManager::isAvailable() {
#ifdef HAVE_LIBSEAT
    return true;
#else
    return false;
#endif
}

#ifdef HAVE_LIBSEAT
void SeatManager::onEnableDevice(struct libseat* seat, void* data) {
    SeatManager* self = static_cast<SeatManager*>(data);
    self->seatEnabled_ = true;
    LOG_INFO << "SeatManager: Seat enabled - devices can now be opened";
    (void)seat;
}

void SeatManager::onDisableDevice(struct libseat* seat, void* data) {
    SeatManager* self = static_cast<SeatManager*>(data);
    self->seatEnabled_ = false;
    LOG_WARNING << "SeatManager: Seat disabled - devices are no longer accessible";
    (void)seat;
}
#endif

} // namespace videocomposer

