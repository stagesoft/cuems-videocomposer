#include "ConfigurationManager.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace videocomposer {

ConfigurationManager::ConfigurationManager() {
    loadDefaults();
}

ConfigurationManager::~ConfigurationManager() {
}

void ConfigurationManager::loadDefaults() {
    // Set default values
    setInt("osc_port", 7000); // Default OSC port
    setBool("remote_en", false);
    setBool("mq_en", false);
    setBool("want_quiet", false);
    setBool("want_verbose", false);
    setDouble("fps", 0.0); // 0 = use file framerate
    setInt("offset", 0);
    setString("midi_port", "-1"); // -1 = autodetect
    setBool("midi_clkadj", false); // MIDI clock adjustment
    setDouble("delay", -1.0); // Frame delay (1.0/fps, or -1 to use file framerate)
    setBool("want_letterbox", true);
    setBool("start_fullscreen", false);
    setBool("start_ontop", false);
    setBool("want_noindex", false); // Index frames by default for frame-accurate seeking
    setString("hardware_decoder", "auto");
}

bool ConfigurationManager::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse key=value pairs
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            
            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            config_[key] = value;
        }
    }

    return true;
}

bool ConfigurationManager::saveToFile(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    file << "# videocomposer configuration file\n";
    file << "# Generated automatically\n\n";

    for (const auto& pair : config_) {
        file << pair.first << "=" << pair.second << "\n";
    }

    return true;
}

int ConfigurationManager::parseCommandLine(int argc, char** argv) {
    arguments_.clear();
    movieFile_.clear();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        arguments_.push_back(arg);

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 1; // Signal to exit
        } else if (arg == "--version" || arg == "-V") {
            printVersion();
            return 1; // Signal to exit
        } else if (arg == "--osc" || arg == "-O") {
            if (i + 1 < argc) {
                setInt("osc_port", std::atoi(argv[++i]));
            } else {
                setInt("osc_port", 7000); // Default port
            }
        } else if (arg == "--remote" || arg == "-R") {
            setBool("remote_en", true);
        } else if (arg == "--mq" || arg == "-Q") {
            setBool("mq_en", true);
        } else if (arg == "--quiet" || arg == "-q") {
            setBool("want_quiet", true);
        } else if (arg == "--verbose" || arg == "-v") {
            setBool("want_verbose", true);
        } else if (arg == "--fps" || arg == "-f") {
            if (i + 1 < argc) {
                setDouble("fps", std::atof(argv[++i]));
            }
        } else if (arg == "--offset" || arg == "-o") {
            if (i + 1 < argc) {
                setInt("offset", std::atoi(argv[++i]));
            }
        } else if (arg == "--midi" || arg == "-m") {
            if (i + 1 < argc) {
                setString("midi_port", argv[++i]);
            }
        } else if (arg == "--midi-clkadj" || arg == "--midi-clk") {
            setBool("midi_clkadj", true);
        } else if (arg == "--fullscreen" || arg == "-s") {
            setBool("start_fullscreen", true);
        } else if (arg == "--ontop" || arg == "-a") {
            setBool("start_ontop", true);
        } else if (arg == "--noindex" || arg == "-n") {
            setBool("want_noindex", true);
        } else if (arg == "--hw-decoder" || arg == "--hw-decode" || arg == "--hw" || arg == "--decode") {
            if (i + 1 < argc) {
                std::string value = argv[++i];
                // store lowercase to simplify comparisons
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
                setString("hardware_decoder", value);
            }
        } else if (arg[0] != '-') {
            // Assume it's a movie file
            if (movieFile_.empty()) {
                movieFile_ = arg;
            }
        }
    }

    return 0;
}

std::string ConfigurationManager::getString(const std::string& key, const std::string& defaultValue) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        return it->second;
    }
    return defaultValue;
}

int ConfigurationManager::getInt(const std::string& key, int defaultValue) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        return std::atoi(it->second.c_str());
    }
    return defaultValue;
}

double ConfigurationManager::getDouble(const std::string& key, double defaultValue) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        return std::atof(it->second.c_str());
    }
    return defaultValue;
}

bool ConfigurationManager::getBool(const std::string& key, bool defaultValue) const {
    auto it = config_.find(key);
    if (it != config_.end()) {
        const std::string& value = it->second;
        return (value == "1" || value == "true" || value == "yes" || value == "on");
    }
    return defaultValue;
}

void ConfigurationManager::setString(const std::string& key, const std::string& value) {
    config_[key] = value;
}

void ConfigurationManager::setInt(const std::string& key, int value) {
    config_[key] = std::to_string(value);
}

void ConfigurationManager::setDouble(const std::string& key, double value) {
    config_[key] = std::to_string(value);
}

void ConfigurationManager::setBool(const std::string& key, bool value) {
    config_[key] = value ? "1" : "0";
}

std::string ConfigurationManager::getConfigFilePath() const {
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.videocomposerrc";
    }
    return ".videocomposerrc";
}

void ConfigurationManager::printUsage() const {
    printf("cuems-videocomposer - Video composer for CUEMS\n");
    printf("\n");
    printf("Usage: cuems-videocomposer [OPTIONS] [VIDEO_FILE]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              display this help and exit\n");
    printf("  -V, --version           print version information and exit\n");
    printf("  -q, --quiet             suppress non-error messages\n");
    printf("  -v, --verbose           enable verbose output\n");
    printf("  -f, --fps FPS           set playback framerate (overrides file framerate)\n");
    printf("  -o, --offset OFFSET    set time offset in frames\n");
    printf("  -m, --midi PORT        specify MIDI port (ALSA Sequencer port ID or -1 for autodetect)\n");
    printf("                         Default: -1 (autodetect, connects to Midi Through if available)\n");
    printf("  --midi-clkadj           enable MIDI clock adjustment\n");
    printf("  -O, --osc PORT         enable OSC remote control on specified port (default: 7000)\n");
    printf("  -R, --remote            enable text-based remote control (stdin/stdout)\n");
    printf("  -Q, --mq                enable message queue remote control\n");
    printf("  -s, --fullscreen        start in fullscreen mode\n");
    printf("  -a, --ontop             start window on top\n");
    printf("  --hw-decode MODE      select hardware decoder: auto (default), software, vaapi, cuda\n");
    printf("\n");
    printf("MIDI Sync:\n");
    printf("  The application uses ALSA Sequencer for MIDI Time Code (MTC) synchronization.\n");
    printf("  On startup, it automatically connects to the 'Midi Through' system port if available.\n");
    printf("  Use 'aconnect -l' to list available MIDI ports.\n");
    printf("\n");
    printf("Remote Control:\n");
    printf("  Control the application via OSC (--osc) or text-based remote API (--remote).\n");
    printf("  All functionality is available through remote control interfaces.\n");
    printf("\n");
    printf("Report bugs to: Ion Reguera <ion@stagelab.coop>\n");
    printf("Website: <https://stagelab.coop>\n");
    printf("\n");
}

void ConfigurationManager::printVersion() const {
    printf("cuems-videocomposer version 0.1.0 (alpha)\n");
    printf("\n");
    printf("Copyright (C) 2024 stagelab.coop\n");
    printf("Ion Reguera <ion@stagelab.coop>\n");
    printf("\n");
    printf("This program is partially based on xjadeo code.\n");
    printf("See individual source files for xjadeo copyright information.\n");
    printf("\n");
    printf("This program is free software; you can redistribute it and/or modify\n");
    printf("it under the terms of the GNU General Public License as published by\n");
    printf("the Free Software Foundation; either version 3, or (at your option)\n");
    printf("any later version.\n");
    printf("\n");
}

} // namespace videocomposer

