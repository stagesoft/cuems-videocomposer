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

#include "TestFramework.h"
#include "../config/ConfigurationManager.h"
#include <cstring>

using namespace videocomposer;
using namespace videocomposer::test;

bool test_ConfigurationManager_Defaults() {
    ConfigurationManager config;
    
    // Test default values
    int oscPort = config.getInt("osc_port", 0);
    TEST_ASSERT_EQ(oscPort, 7000); // Default OSC port
    
    bool letterbox = config.getBool("want_letterbox", false);
    TEST_ASSERT_TRUE(letterbox); // Default letterbox enabled
    
    return true;
}

bool test_ConfigurationManager_SetGet() {
    ConfigurationManager config;
    
    // Test string
    config.setString("test_string", "hello");
    std::string value = config.getString("test_string", "");
    TEST_ASSERT_EQ(value, "hello");
    
    // Test int
    config.setInt("test_int", 42);
    int intValue = config.getInt("test_int", 0);
    TEST_ASSERT_EQ(intValue, 42);
    
    // Test bool
    config.setBool("test_bool", true);
    bool boolValue = config.getBool("test_bool", false);
    TEST_ASSERT_TRUE(boolValue);
    
    return true;
}

bool test_ConfigurationManager_Override() {
    ConfigurationManager config;
    
    config.setInt("test_value", 10);
    TEST_ASSERT_EQ(config.getInt("test_value", 0), 10);
    
    config.setInt("test_value", 20);
    TEST_ASSERT_EQ(config.getInt("test_value", 0), 20);
    
    return true;
}

bool test_ConfigurationManager_NonExistent() {
    ConfigurationManager config;
    
    // Non-existent values should return defaults
    std::string str = config.getString("nonexistent", "default");
    TEST_ASSERT_EQ(str, "default");
    
    int i = config.getInt("nonexistent", 99);
    TEST_ASSERT_EQ(i, 99);
    
    bool b = config.getBool("nonexistent", false);
    TEST_ASSERT_FALSE(b);
    
    return true;
}

