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

#ifndef VIDEOCOMPOSER_LOGGER_H
#define VIDEOCOMPOSER_LOGGER_H

#include <string>
#include <iostream>
#include <sstream>
#include <cstdlib>

// Under systemd every stdout/stderr line is stamped PRIORITY=6 (info)
// regardless of severity, so `journalctl -p 4` / `cuems-logs -e` could never
// surface a single one of this program's LOG_ERROR/LOG_WARNING lines — 36k+
// journal entries/day, every one recorded as info. When HAVE_CUEMS_LOGGER is
// defined (the cuems-videocomposer target; videoindexer and the test binaries
// deliberately stay on plain streams) the five emit bodies below route
// through CuemsLogger, whose openlog/syslog path carries the record's real
// priority. Terminal runs are unaffected: when JOURNAL_STREAM is absent
// (systemd sets it exactly when stdout/stderr are wired to the journal) the
// original cerr/cout output is kept byte-for-byte.
#ifdef HAVE_CUEMS_LOGGER
#include "cuemslogger.h"
#endif

namespace videocomposer {

/**
 * Logger - Simple logging utility for C++ code
 * 
 * Provides consistent logging interface that can integrate with
 * existing C code's logging (want_quiet, want_verbose, want_debug).
 */
class Logger {
public:
    enum Level {
        ERROR = 0,
        WARNING = 1,
        INFO = 2,
        DEBUG = 3,
        VERBOSE = 4
    };

    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLevel(Level level) { level_ = level; }
    Level getLevel() const { return level_; }

    void setQuiet(bool quiet) { quiet_ = quiet; }
    bool isQuiet() const { return quiet_; }

    // Logging methods. The !quiet_/level_ guards are the volume control and
    // the --quiet CLI contract — they gate BEFORE any backend is touched, so
    // level filtering costs the same whichever backend is active.
    void error(const std::string& message) {
        if (!quiet_ && level_ >= ERROR) {
#ifdef HAVE_CUEMS_LOGGER
            if (underJournal()) { CuemsLogger::getLogger()->logError(message); return; }
#endif
            std::cerr << "[ERROR] " << message << std::endl;
        }
    }

    void warning(const std::string& message) {
        if (!quiet_ && level_ >= WARNING) {
#ifdef HAVE_CUEMS_LOGGER
            if (underJournal()) { CuemsLogger::getLogger()->logWarning(message); return; }
#endif
            std::cerr << "[WARNING] " << message << std::endl;
        }
    }

    void info(const std::string& message) {
        if (!quiet_ && level_ >= INFO) {
#ifdef HAVE_CUEMS_LOGGER
            if (underJournal()) { CuemsLogger::getLogger()->logInfo(message); return; }
#endif
            std::cout << "[INFO] " << message << std::endl;
        }
    }

    void debug(const std::string& message) {
        if (!quiet_ && level_ >= DEBUG) {
#ifdef HAVE_CUEMS_LOGGER
            if (underJournal()) { CuemsLogger::getLogger()->logDebug(message); return; }
#endif
            std::cout << "[DEBUG] " << message << std::endl;
        }
    }

    void verbose(const std::string& message) {
        if (!quiet_ && level_ >= VERBOSE) {
#ifdef HAVE_CUEMS_LOGGER
            // CuemsLogger has no level below debug; VERBOSE folds into it.
            if (underJournal()) { CuemsLogger::getLogger()->logDebug(message); return; }
#endif
            std::cout << "[VERBOSE] " << message << std::endl;
        }
    }

    // Convenience macros (can be used like: LOG_INFO << "message")
    class LogStream {
    public:
        LogStream(Logger& logger, Level level, bool shouldLog)
            : logger_(logger), level_(level), shouldLog_(shouldLog) {}
        
        // Move constructor (needed because ostringstream is not copyable)
        LogStream(LogStream&& other) noexcept
            : logger_(other.logger_), level_(other.level_), shouldLog_(other.shouldLog_),
              stream_(std::move(other.stream_)) {}
        
        // Delete copy constructor
        LogStream(const LogStream&) = delete;
        LogStream& operator=(const LogStream&) = delete;
        
        ~LogStream() {
            if (shouldLog_) {
                std::string msg = stream_.str();
                switch (level_) {
                    case ERROR: logger_.error(msg); break;
                    case WARNING: logger_.warning(msg); break;
                    case INFO: logger_.info(msg); break;
                    case DEBUG: logger_.debug(msg); break;
                    case VERBOSE: logger_.verbose(msg); break;
                }
            }
        }

        template<typename T>
        LogStream& operator<<(const T& value) {
            if (shouldLog_) {
                stream_ << value;
            }
            return *this;
        }

    private:
        Logger& logger_;
        Level level_;
        bool shouldLog_;
        std::ostringstream stream_;
    };

    LogStream error() { return LogStream(*this, ERROR, !quiet_ && level_ >= ERROR); }
    LogStream warning() { return LogStream(*this, WARNING, !quiet_ && level_ >= WARNING); }
    LogStream info() { return LogStream(*this, INFO, !quiet_ && level_ >= INFO); }
    LogStream debug() { return LogStream(*this, DEBUG, !quiet_ && level_ >= DEBUG); }
    LogStream verbose() { return LogStream(*this, VERBOSE, !quiet_ && level_ >= VERBOSE); }

private:
    Logger() : level_(INFO), quiet_(false) {}
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

#ifdef HAVE_CUEMS_LOGGER
    static bool underJournal() {
        static const bool v = (std::getenv("JOURNAL_STREAM") != nullptr);
        return v;
    }
#endif

    Level level_;
    bool quiet_;
};

// Convenience macros.
//
// <syslog.h> (reached via cuemslogger.h) defines LOG_WARNING/LOG_INFO/
// LOG_DEBUG as integer priority macros. Undefine them before establishing
// this file's stream macros, or every TU warns "redefined" — and a TU that
// includes syslog.h AFTER this header would flip LOG_WARNING back to `4`,
// turning `LOG_WARNING << "..."` into a nonsense integer shift. syslog.h is
// include-guarded, so once undefined here the stream macros stay in force
// for the rest of the TU regardless of later includes. (Same dance
// rtpmidid's logger.hpp and gradient-motion-engine's logging.h do.)
#ifdef LOG_WARNING
#undef LOG_WARNING
#endif
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif
#define LOG_ERROR   videocomposer::Logger::getInstance().error()
#define LOG_WARNING videocomposer::Logger::getInstance().warning()
#define LOG_INFO    videocomposer::Logger::getInstance().info()
#define LOG_DEBUG   videocomposer::Logger::getInstance().debug()
#define LOG_VERBOSE videocomposer::Logger::getInstance().verbose()

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LOGGER_H

