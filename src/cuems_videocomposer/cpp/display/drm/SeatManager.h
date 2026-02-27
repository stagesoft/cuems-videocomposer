/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
    bool seatEnabled_ = false;  // Track if seat is enabled (can open devices)
    
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

