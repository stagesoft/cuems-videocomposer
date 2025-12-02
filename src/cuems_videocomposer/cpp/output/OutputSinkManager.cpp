/**
 * OutputSinkManager.cpp - Virtual output management implementation
 */

#include "OutputSinkManager.h"
#include "../utils/Logger.h"

namespace videocomposer {

OutputSinkManager::OutputSinkManager() {
}

OutputSinkManager::~OutputSinkManager() {
    closeAll();
    clear();
}

bool OutputSinkManager::addSink(std::unique_ptr<OutputSink> sink) {
    if (!sink) {
        LOG_ERROR << "OutputSinkManager: Cannot add null sink";
        return false;
    }
    
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    // Check for duplicate ID
    std::string id = sink->getId();
    for (const auto& existing : sinks_) {
        if (existing->getId() == id) {
            LOG_ERROR << "OutputSinkManager: Sink with ID '" << id << "' already exists";
            return false;
        }
    }
    
    LOG_INFO << "OutputSinkManager: Added sink '" << id 
             << "' (" << sink->getDescription() << ")";
    
    sinks_.push_back(std::move(sink));
    return true;
}

bool OutputSinkManager::removeSink(const std::string& id) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
        if ((*it)->getId() == id) {
            (*it)->close();
            LOG_INFO << "OutputSinkManager: Removed sink '" << id << "'";
            sinks_.erase(it);
            return true;
        }
    }
    
    return false;
}

OutputSink* OutputSinkManager::getSink(const std::string& id) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto& sink : sinks_) {
        if (sink->getId() == id) {
            return sink.get();
        }
    }
    
    return nullptr;
}

OutputSink* OutputSinkManager::getSinkByType(OutputSink::Type type) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto& sink : sinks_) {
        if (sink->getType() == type) {
            return sink.get();
        }
    }
    
    return nullptr;
}

std::vector<OutputSink*> OutputSinkManager::getSinksByType(OutputSink::Type type) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    std::vector<OutputSink*> result;
    for (auto& sink : sinks_) {
        if (sink->getType() == type) {
            result.push_back(sink.get());
        }
    }
    
    return result;
}

void OutputSinkManager::writeFrameToAll(const FrameData& frame) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto& sink : sinks_) {
        if (sink->isReady()) {
            sink->writeFrame(frame);
        }
    }
}

bool OutputSinkManager::writeFrameToSink(const std::string& id, const FrameData& frame) {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto& sink : sinks_) {
        if (sink->getId() == id) {
            if (sink->isReady()) {
                return sink->writeFrame(frame);
            }
            return false;
        }
    }
    
    return false;
}

bool OutputSinkManager::hasActiveSinks() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (const auto& sink : sinks_) {
        if (sink->isReady()) {
            return true;
        }
    }
    
    return false;
}

size_t OutputSinkManager::getActiveSinkCount() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    size_t count = 0;
    for (const auto& sink : sinks_) {
        if (sink->isReady()) {
            ++count;
        }
    }
    
    return count;
}

std::vector<std::string> OutputSinkManager::getActiveSinkIds() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    std::vector<std::string> ids;
    for (const auto& sink : sinks_) {
        if (sink->isReady()) {
            ids.push_back(sink->getId());
        }
    }
    
    return ids;
}

std::vector<std::string> OutputSinkManager::getAllSinkIds() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    std::vector<std::string> ids;
    for (const auto& sink : sinks_) {
        ids.push_back(sink->getId());
    }
    
    return ids;
}

void OutputSinkManager::closeAll() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    for (auto& sink : sinks_) {
        if (sink->isReady()) {
            sink->close();
        }
    }
    
    LOG_INFO << "OutputSinkManager: Closed all sinks";
}

void OutputSinkManager::clear() {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    sinks_.clear();
    LOG_INFO << "OutputSinkManager: Cleared all sinks";
}

int64_t OutputSinkManager::getTotalFramesWritten() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    int64_t total = 0;
    for (const auto& sink : sinks_) {
        total += sink->getFramesWritten();
    }
    
    return total;
}

int64_t OutputSinkManager::getTotalFramesDropped() const {
    std::lock_guard<std::mutex> lock(sinksMutex_);
    
    int64_t total = 0;
    for (const auto& sink : sinks_) {
        total += sink->getFramesDropped();
    }
    
    return total;
}

} // namespace videocomposer

