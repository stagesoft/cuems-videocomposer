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
#include <iostream>

int main(int argc, char** argv) {
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
