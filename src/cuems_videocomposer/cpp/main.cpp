/*
 * cuems-videocomposer - Video composer for CUEMS
 *
 * Copyright (C) 2024 stagelab.coop
 * Ion Reguera <ion@stagelab.coop>
 *
 * This program is partially based on xjadeo code:
 * Copyright (C) 2005-2014 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2010-2012 Fons Adriaensen <fons@linuxaudio.org>
 * Copyright (C) 2009-2010 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2005-2008 Jörn Nettingsmeier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "VideoComposerApplication.h"
#include "input/NDIVideoInput.h"
#include <iostream>

int main(int argc, char** argv) {
    // Check for --discover-ndi flag before full initialization
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--discover-ndi" || arg == "--list-ndi") {
#ifdef HAVE_NDI_SDK
            int timeout = 5; // Default 5 seconds
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                timeout = std::atoi(argv[i + 1]);
            }
            
            std::cout << "NDI Source Discovery" << std::endl;
            std::cout << "=" << std::string(50, '=') << std::endl;
            std::cout << "Timeout: " << timeout << " seconds" << std::endl;
            std::cout << "Searching for NDI sources..." << std::endl;
            std::cout << std::endl;
            
            auto sources = videocomposer::NDIVideoInput::discoverSources(timeout * 1000);
            
            if (sources.empty()) {
                std::cout << "No NDI sources found." << std::endl;
                std::cout << std::endl;
                std::cout << "Troubleshooting:" << std::endl;
                std::cout << "  1. Ensure an NDI source is running (NDI Test Patterns, OBS, etc.)" << std::endl;
                std::cout << "  2. Check network connectivity" << std::endl;
                std::cout << "  3. Verify firewall allows NDI (UDP ports 5960-5969)" << std::endl;
                std::cout << "  4. Try increasing timeout: --discover-ndi 10" << std::endl;
                return 1;
            }
            
            std::cout << "Found " << sources.size() << " NDI source(s):" << std::endl;
            std::cout << std::endl;
            for (size_t i = 0; i < sources.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << sources[i] << std::endl;
            }
            std::cout << std::endl;
            std::cout << "To use with videocomposer:" << std::endl;
            std::cout << "  ./cuems-videocomposer \"ndi://" << sources[0] << "\"" << std::endl;
            return 0;
#else
            std::cerr << "ERROR: NDI SDK not available. Build with -DENABLE_NDI=ON" << std::endl;
            return 1;
#endif
        }
    }
    
    videocomposer::VideoComposerApplication app;
    
    if (!app.initialize(argc, argv)) {
        // Check if help or version was requested (they return false but print output)
        // In that case, exit successfully. Otherwise it's a real error.
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h" || arg == "--version" || arg == "-V") {
                return 0; // Help/version already printed, exit successfully
            }
        }
        // Real initialization error
        return 1;
    }
    
    return app.run();
}
