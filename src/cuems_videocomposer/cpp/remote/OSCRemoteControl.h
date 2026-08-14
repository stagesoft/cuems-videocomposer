/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_OSCREMOTECONTROL_H
#define VIDEOCOMPOSER_OSCREMOTECONTROL_H

#include "RemoteControl.h"
#include "RemoteCommandRouter.h"
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lo/lo_lowlevel.h>
}

namespace videocomposer {

// Forward declarations
class VideoComposerApplication;
class LayerManager;

/**
 * OSCRemoteControl - OSC (Open Sound Control) remote control implementation
 * 
 * Implements RemoteControl interface using liblo for OSC protocol.
 * This is the only remote control implementation for now, but the architecture
 * is ready for future implementations (MessageQueue, IPC, etc.).
 */
class OSCRemoteControl : public RemoteControl {
public:
    OSCRemoteControl(VideoComposerApplication* app, LayerManager* layerManager);
    virtual ~OSCRemoteControl();

    // RemoteControl interface
    bool initialize(int port) override;
    int process() override;
    void shutdown() override;
    bool isActive() const override;
    const char* getProtocolName() const override { return "OSC"; }

private:
    // OSC server
    lo_server oscServer_;
    std::unique_ptr<RemoteCommandRouter> router_;

    // OSC user data (for callbacks)
    struct OSCUserData {
        OSCRemoteControl* instance;
    };
    OSCUserData* userData_;

    // OSC message handlers (static callbacks that forward to instance)
    static int handleOSCMessage(const char* path, const char* types, 
                                 lo_arg** argv, int argc, lo_message msg, 
                                 void* userData);
    
    // Convert OSC arguments to string vector
    std::vector<std::string> convertOSCArgs(const char* types, lo_arg** argv, int argc);

    // Port
    int port_;
    bool active_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OSCREMOTECONTROL_H

