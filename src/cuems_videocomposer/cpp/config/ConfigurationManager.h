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

#ifndef VIDEOCOMPOSER_CONFIGURATIONMANAGER_H
#define VIDEOCOMPOSER_CONFIGURATIONMANAGER_H

#include <string>
#include <map>
#include <vector>

namespace videocomposer {

/**
 * ConfigurationManager - Handles configuration file reading/writing and command-line parsing
 * 
 * Manages application configuration, command-line options, and config file persistence.
 */
class ConfigurationManager {
public:
    ConfigurationManager();
    ~ConfigurationManager();

    // Load configuration from file
    bool loadFromFile(const std::string& filename);
    
    // Save configuration to file
    bool saveToFile(const std::string& filename) const;

    // Parse command-line arguments
    int parseCommandLine(int argc, char** argv);

    // Get configuration values
    std::string getString(const std::string& key, const std::string& defaultValue = "") const;
    int getInt(const std::string& key, int defaultValue = 0) const;
    double getDouble(const std::string& key, double defaultValue = 0.0) const;
    bool getBool(const std::string& key, bool defaultValue = false) const;

    // Set configuration values
    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setDouble(const std::string& key, double value);
    void setBool(const std::string& key, bool value);

    // Get command-line arguments (for compatibility)
    std::vector<std::string> getArguments() const { return arguments_; }
    std::string getMovieFile() const { return movieFile_; }
    std::string getConfigFilePath() const;
    
    // Print usage/help information
    void printUsage() const;
    
    // Print version information
    void printVersion() const;

    // Whether -r/--resolution was explicitly passed on the command line
    bool isResolutionExplicit() const { return resolutionExplicit_; }

private:
    std::map<std::string, std::string> config_;
    std::vector<std::string> arguments_;
    std::string movieFile_;
    bool resolutionExplicit_ = false;
    
    void loadDefaults();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CONFIGURATIONMANAGER_H

