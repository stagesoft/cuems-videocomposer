/**
 * OutputSinkManager.h - Manages multiple virtual video outputs
 * 
 * Shared component for Multi-Display and NDI implementations.
 * Provides centralized management for all virtual output sinks
 * including NDI, streaming, and file recording.
 */

#ifndef VIDEOCOMPOSER_OUTPUTSINKMANAGER_H
#define VIDEOCOMPOSER_OUTPUTSINKMANAGER_H

#include "OutputSink.h"
#include <vector>
#include <memory>
#include <mutex>
#include <string>

namespace videocomposer {

/**
 * OutputSinkManager - Central manager for virtual outputs
 * 
 * Manages all virtual output sinks and provides:
 * - Sink registration and removal
 * - Broadcasting frames to all active sinks
 * - Thread-safe frame distribution
 * - Status reporting
 */
class OutputSinkManager {
public:
    OutputSinkManager();
    ~OutputSinkManager();
    
    // ===== Sink Management =====
    
    /**
     * Add an output sink
     * @param sink Sink to add (ownership transferred)
     * @return true on success
     */
    bool addSink(std::unique_ptr<OutputSink> sink);
    
    /**
     * Remove a sink by ID
     * @param id Sink ID to remove
     * @return true if found and removed
     */
    bool removeSink(const std::string& id);
    
    /**
     * Get a sink by ID
     * @param id Sink ID
     * @return Pointer to sink, or nullptr if not found
     */
    OutputSink* getSink(const std::string& id);
    
    /**
     * Get sink by type
     * @param type Sink type to find
     * @return First matching sink, or nullptr
     */
    OutputSink* getSinkByType(OutputSink::Type type);
    
    /**
     * Get all sinks of a type
     */
    std::vector<OutputSink*> getSinksByType(OutputSink::Type type);
    
    // ===== Frame Distribution =====
    
    /**
     * Write a frame to all active sinks
     * Thread-safe - can be called from render thread
     * @param frame Frame data to write
     */
    void writeFrameToAll(const FrameData& frame);
    
    /**
     * Write a frame to a specific sink
     * @param id Sink ID
     * @param frame Frame data
     * @return true if sink found and frame written
     */
    bool writeFrameToSink(const std::string& id, const FrameData& frame);
    
    // ===== Status =====
    
    /**
     * Check if any sinks are active
     */
    bool hasActiveSinks() const;
    
    /**
     * Get number of active sinks
     */
    size_t getActiveSinkCount() const;
    
    /**
     * Get all active sink IDs
     */
    std::vector<std::string> getActiveSinkIds() const;
    
    /**
     * Get all sink IDs (active and inactive)
     */
    std::vector<std::string> getAllSinkIds() const;
    
    // ===== Lifecycle =====
    
    /**
     * Close all sinks
     */
    void closeAll();
    
    /**
     * Remove all sinks
     */
    void clear();
    
    // ===== Statistics =====
    
    /**
     * Get total frames written across all sinks
     */
    int64_t getTotalFramesWritten() const;
    
    /**
     * Get total frames dropped across all sinks
     */
    int64_t getTotalFramesDropped() const;
    
private:
    std::vector<std::unique_ptr<OutputSink>> sinks_;
    mutable std::mutex sinksMutex_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTSINKMANAGER_H

