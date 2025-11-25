# Approaches for Single File Read: Evaluation

## Goal

Enable both `cuems-audioplayer` and `cuems-videocomposer` to read the same file with minimal duplication, ideally reading from disk only once.

## Context

From previous evaluations:
- Both processes currently read file independently
- OS file cache helps, but doesn't eliminate duplicate reads
- Timecode synchronization means both access same file regions
- Index data could be shared

## Approach 1: Shared Memory Buffer (mmap)

### Concept

Memory-map the entire file into shared memory. Both processes access the same physical memory pages.

### Implementation

```cpp
// In cuems-mediadecoder or a shared component
class SharedFileBuffer {
public:
    bool open(const std::string& path) {
        // Open file
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        
        // Get file size
        struct stat st;
        fstat(fd, &st);
        file_size_ = st.st_size;
        
        // Memory map with MAP_SHARED
        buffer_ = mmap(NULL, file_size_, PROT_READ, MAP_SHARED, fd, 0);
        if (buffer_ == MAP_FAILED) {
            close(fd);
            return false;
        }
        
        fd_ = fd;
        return true;
    }
    
    void* getBuffer() { return buffer_; }
    size_t getSize() { return file_size_; }
    
private:
    void* buffer_;
    size_t file_size_;
    int fd_;
};

// Custom AVIOContext for FFmpeg
static int read_packet(void* opaque, uint8_t* buf, int buf_size) {
    SharedFileBuffer* buffer = static_cast<SharedFileBuffer*>(opaque);
    // Read from shared memory buffer
    // ...
}
```

### Pros

✅ **True Single Read**: File read into memory once, both processes share
✅ **Zero-Copy**: Both processes access same physical memory
✅ **OS Handles Sharing**: `MAP_SHARED` automatically shares pages
✅ **Efficient**: No data duplication in RAM
✅ **Fast Access**: Memory access faster than file I/O

### Cons

❌ **Memory Usage**: Entire file in RAM (problematic for large files)
❌ **FFmpeg Complexity**: Need custom `AVIOContext` implementation
❌ **Seeking Complexity**: Must implement seek in custom I/O context
❌ **Not Suitable for Large Files**: 10GB file = 10GB RAM
❌ **Network Files**: Doesn't work well with network storage
❌ **Live Streams**: Can't memory-map live streams

### Memory Impact

**Example**: 5GB video file
- **Current**: 2 processes × (OS cache + buffers) ≈ 5GB + overhead
- **mmap**: 1 × 5GB in shared memory ≈ 5GB total
- **Savings**: Minimal (OS cache already efficient)

### Complexity

**High** - Requires:
- Custom `AVIOContext` implementation
- Seek implementation
- Buffer management
- Error handling
- Testing with various file formats

### Verdict

⚠️ **Not Recommended** - High complexity, minimal benefit, memory issues with large files

---

## Approach 2: Dedicated Reader Process (Media Server)

### Concept

Single dedicated process reads the file. Other processes request data via IPC.

### Architecture

```
┌─────────────────────┐
│  Media Server       │
│  - Reads file       │
│  - Maintains index  │
│  - Serves packets   │
└──────────┬──────────┘
           │ IPC (sockets/pipes)
           │
    ┌──────┴──────┐
    │             │
┌───▼───┐    ┌───▼───┐
│Video  │    │Audio  │
│Player │    │Player │
└───────┘    └───────┘
```

### Implementation

```cpp
// Media Server (new process)
class MediaServer {
    void serveRequests() {
        while (running) {
            // Read packet from file
            AVPacket* packet = readNextPacket();
            
            // Broadcast to all clients
            for (auto& client : clients) {
                sendPacket(client, packet);
            }
        }
    }
};

// Client (cuems-videocomposer, cuems-audioplayer)
class MediaClient {
    AVPacket* requestPacket(int64_t timestamp) {
        // Send request to server
        sendRequest(timestamp);
        
        // Receive packet from server
        return receivePacket();
    }
};
```

### Pros

✅ **True Single Read**: One process reads file
✅ **Shared Index**: Server maintains index, clients query it
✅ **Coordinated Access**: Server can optimize I/O
✅ **Works with Large Files**: Server manages memory
✅ **Network Compatible**: Server can handle network files

### Cons

❌ **High Complexity**: New process, IPC, synchronization
❌ **Latency**: IPC adds delay (may affect real-time playback)
❌ **Single Point of Failure**: Server crash affects all clients
❌ **Synchronization**: Need to coordinate seeks, playback
❌ **Timecode Sync**: Complex with external timecode
❌ **Performance**: IPC overhead for every packet

### IPC Options

1. **Unix Domain Sockets**: Fast, local only
2. **Shared Memory + Semaphores**: Very fast, complex
3. **Message Queues**: Simple, moderate performance
4. **TCP/IP**: Network-capable, slower

### Complexity

**Very High** - Requires:
- New server process
- IPC protocol design
- Packet buffering
- Client-server synchronization
- Error handling and recovery
- Process lifecycle management

### Verdict

❌ **Not Recommended** - Too complex, latency issues, single point of failure

---

## Approach 3: File-Based Index + Coordinated Reading

### Concept

Share index file. Coordinate reads to minimize duplication (read-ahead buffer).

### Implementation

```cpp
// Shared index file (video.mp4.idx)
struct SharedIndex {
    int64_t frameCount;
    FrameIndex entries[];
};

// Reader coordination via lock file
class CoordinatedReader {
    bool requestRead(int64_t position, size_t size) {
        // Check if another process is reading nearby
        if (isNearbyRead(position)) {
            // Wait for other process to finish
            // Then read from OS cache
            return readFromCache(position, size);
        }
        
        // Read from disk
        return readFromDisk(position, size);
    }
};
```

### Pros

✅ **Simple**: File-based coordination
✅ **Shared Index**: Both processes use same index
✅ **OS Cache Benefits**: Second reader gets cached data
✅ **Works with Current Architecture**: Minimal changes
✅ **No New Process**: Uses existing processes

### Cons

⚠️ **Not True Single Read**: Still two reads, but cached
⚠️ **Coordination Overhead**: Lock file management
⚠️ **Race Conditions**: Need careful synchronization
⚠️ **Limited Benefit**: OS cache already does this

### Complexity

**Low-Medium** - Requires:
- Index file format
- Lock file mechanism
- Read coordination logic
- Error handling

### Verdict

⚠️ **Limited Benefit** - OS cache already provides most benefits

---

## Approach 4: Memory-Mapped I/O with FFmpeg Custom AVIOContext

### Concept

Use `mmap` for file, create custom `AVIOContext` that reads from mapped memory.

### Implementation

```cpp
// Custom AVIOContext
static int read_packet(void* opaque, uint8_t* buf, int buf_size) {
    MappedFileContext* ctx = static_cast<MappedFileContext*>(opaque);
    
    if (ctx->pos >= ctx->size) {
        return AVERROR_EOF;
    }
    
    size_t to_read = std::min(buf_size, ctx->size - ctx->pos);
    memcpy(buf, (uint8_t*)ctx->mapped + ctx->pos, to_read);
    ctx->pos += to_read;
    
    return to_read;
}

static int64_t seek(void* opaque, int64_t offset, int whence) {
    MappedFileContext* ctx = static_cast<MappedFileContext*>(opaque);
    
    switch (whence) {
        case SEEK_SET: ctx->pos = offset; break;
        case SEEK_CUR: ctx->pos += offset; break;
        case SEEK_END: ctx->pos = ctx->size + offset; break;
    }
    
    return ctx->pos;
}

// Usage
AVIOContext* createMappedIO(const std::string& path) {
    // Map file
    int fd = open(path.c_str(), O_RDONLY);
    void* mapped = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    
    // Create custom I/O context
    MappedFileContext* ctx = new MappedFileContext(mapped, file_size);
    
    unsigned char* buffer = (unsigned char*)av_malloc(4096);
    AVIOContext* avio = avio_alloc_context(
        buffer, 4096, 0, ctx, read_packet, NULL, seek
    );
    
    return avio;
}
```

### Pros

✅ **Single Physical Read**: File mapped once, shared
✅ **FFmpeg Compatible**: Uses standard AVIOContext
✅ **Zero-Copy Potential**: Can pass pointers directly
✅ **Works with Seeking**: Custom seek implementation

### Cons

❌ **Memory Usage**: Entire file in RAM
❌ **Large Files**: Problematic for multi-GB files
❌ **Implementation Complexity**: Custom AVIOContext
❌ **Network Files**: Doesn't work with network storage
❌ **Live Streams**: Can't map live streams

### Complexity

**Medium-High** - Requires:
- Custom AVIOContext implementation
- Memory mapping management
- Seek implementation
- Error handling
- Testing

### Verdict

⚠️ **Not Recommended for Large Files** - Good for small files, problematic for large ones

---

## Approach 5: Hybrid: Shared Index + Smart Caching

### Concept

Share index file. Implement read-ahead buffer coordination. Use OS cache effectively.

### Implementation

```cpp
// Shared index file
class SharedIndex {
    static bool loadOrCreate(const std::string& videoFile) {
        std::string idxFile = videoFile + ".idx";
        
        // Try to load existing index
        if (fileExists(idxFile)) {
            return loadFromFile(idxFile);
        }
        
        // Create index (first process)
        if (createIndex(videoFile, idxFile)) {
            return true;
        }
        
        // Wait for other process to create it
        return waitForIndex(idxFile);
    }
};

// Smart read coordination
class SmartReader {
    void readWithCoordination(int64_t position) {
        // Check if another process recently read nearby
        if (recentReadNearby(position)) {
            // Small delay to let OS cache populate
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        // Read (will hit OS cache if available)
        readFromFile(position);
    }
};
```

### Pros

✅ **Simple**: File-based, no new processes
✅ **Shared Index**: Both processes benefit
✅ **OS Cache Optimization**: Coordinates to maximize cache hits
✅ **Works with Current Architecture**: Minimal changes
✅ **No Memory Issues**: Doesn't require entire file in RAM
✅ **Network Compatible**: Works with network storage

### Cons

⚠️ **Not True Single Read**: Still two reads, but optimized
⚠️ **Coordination Overhead**: Some complexity
⚠️ **Timing Dependent**: Relies on OS cache timing

### Complexity

**Low-Medium** - Requires:
- Index file format
- Read coordination logic
- Cache timing optimization

### Verdict

✅ **Recommended** - Best balance of simplicity and benefit

---

## Approach 6: Process Fork with Shared Memory

### Concept

Parent process reads file, forks children. Children inherit file descriptor and can share memory.

### Implementation

```cpp
// Parent process
int main() {
    // Open file
    int fd = open("video.mp4", O_RDONLY);
    
    // Map file
    void* mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    
    // Fork children
    if (fork() == 0) {
        // Child 1: Video player
        runVideoPlayer(mapped, size);
    }
    if (fork() == 0) {
        // Child 2: Audio player
        runAudioPlayer(mapped, size);
    }
    
    // Parent waits
    wait(NULL);
}
```

### Pros

✅ **True Single Read**: Parent reads once
✅ **Shared Memory**: Children share mapped memory
✅ **Simple IPC**: Inherited file descriptors

### Cons

❌ **Architecture Change**: Requires single parent process
❌ **Not Suitable**: Current architecture has separate processes
❌ **Deployment Complexity**: Need to launch from parent
❌ **Memory Issues**: Still requires entire file in RAM

### Verdict

❌ **Not Suitable** - Requires major architecture change

---

## Comparison Table

| Approach | Single Read | Complexity | Memory | Large Files | Network | Recommendation |
|----------|-------------|------------|--------|-------------|---------|----------------|
| **1. mmap Shared Buffer** | ✅ Yes | High | High | ❌ No | ❌ No | ⚠️ Not recommended |
| **2. Dedicated Server** | ✅ Yes | Very High | Medium | ✅ Yes | ✅ Yes | ❌ Too complex |
| **3. Coordinated Reading** | ⚠️ Partial | Low-Medium | Low | ✅ Yes | ✅ Yes | ⚠️ Limited benefit |
| **4. Custom AVIOContext** | ✅ Yes | Medium-High | High | ❌ No | ❌ No | ⚠️ Not for large files |
| **5. Hybrid (Index + Cache)** | ⚠️ Partial | Low-Medium | Low | ✅ Yes | ✅ Yes | ✅ **Recommended** |
| **6. Process Fork** | ✅ Yes | Medium | High | ❌ No | ❌ No | ❌ Architecture change |

## Detailed Evaluation: Hybrid Approach (Recommended)

### Why It's Best

1. **Practical Single Read**:
   - First process reads from disk
   - Second process reads from OS cache (effectively single read)
   - Timecode sync means both read same regions

2. **Shared Index**:
   - Video player creates index file
   - Audio player loads index file
   - Both benefit from optimized seeking

3. **Simple Implementation**:
   - File-based (no IPC, no new processes)
   - Works with current architecture
   - Minimal changes needed

4. **Works with Large Files**:
   - Doesn't require entire file in RAM
   - OS cache handles large files efficiently
   - Network storage compatible

### Implementation Details

#### 1. Index File Format

```cpp
// video.mp4.idx
struct IndexFileHeader {
    uint32_t magic;           // "IDX\0"
    uint32_t version;          // Format version
    uint64_t frameCount;       // Number of frames
    int64_t fileFrameOffset;   // File frame offset
    // ... metadata
};

struct IndexFileEntry {
    int64_t pkt_pts;
    int64_t pkt_pos;
    int64_t frame_pts;
    int64_t frame_pos;
    int64_t timestamp;
    int64_t seekpts;
    int64_t seekpos;
    uint8_t key;
};
```

#### 2. Index Creation/Sharing

```cpp
// In VideoFileInput::indexFrames()
bool VideoFileInput::indexFrames() {
    // ... existing indexing code ...
    
    // Save index to file
    std::string idxFile = currentFile_ + ".idx";
    if (saveIndexToFile(idxFile, frameIndex_, frameCount_)) {
        LOG_INFO << "Index saved to " << idxFile;
    }
    
    return true;
}

// In AudioPlayer (or VideoFileInput on second open)
bool loadSharedIndex(const std::string& videoFile) {
    std::string idxFile = videoFile + ".idx";
    
    // Check if index exists
    if (!fileExists(idxFile)) {
        return false; // No index available
    }
    
    // Check if index is recent (not stale)
    if (isIndexStale(idxFile, videoFile)) {
        return false; // Index outdated
    }
    
    // Load index
    return loadIndexFromFile(idxFile, &frameIndex_, &frameCount_);
}
```

#### 3. Read Coordination

```cpp
// Optional: Coordinate reads to maximize cache hits
class ReadCoordinator {
    struct RecentRead {
        int64_t position;
        std::chrono::time_point<std::chrono::steady_clock> time;
    };
    
    std::vector<RecentRead> recentReads_;
    std::mutex mutex_;
    
    void coordinateRead(int64_t position, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Check if another process recently read nearby
        auto now = std::chrono::steady_clock::now();
        for (auto& read : recentReads_) {
            if (std::abs(read.position - position) < 1024 * 1024) { // 1MB
                auto age = now - read.time;
                if (age < std::chrono::milliseconds(100)) {
                    // Recent read nearby - small delay to let cache populate
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    break;
                }
            }
        }
        
        // Record this read
        recentReads_.push_back({position, now});
        
        // Clean old entries
        recentReads_.erase(
            std::remove_if(recentReads_.begin(), recentReads_.end(),
                [now](const RecentRead& r) {
                    return (now - r.time) > std::chrono::seconds(1);
                }),
            recentReads_.end()
        );
    }
};
```

### Benefits

1. **Shared Index**:
   - Video player creates index once
   - Audio player uses same index
   - Both get optimized seeking

2. **OS Cache Optimization**:
   - First process reads from disk
   - Second process reads from cache
   - Timecode sync maximizes cache hits

3. **Simple**:
   - File-based (no IPC)
   - Works with current architecture
   - Easy to implement

4. **Robust**:
   - No single point of failure
   - Works with network storage
   - Handles large files

### Limitations

⚠️ **Not True Single Read**: Still two reads, but second is from cache
⚠️ **Timing Dependent**: Relies on OS cache being populated
⚠️ **Index Staleness**: Need to handle index file updates

### Implementation Priority

1. **Phase 1** (High Priority): Index file sharing
   - Save index to `.idx` file
   - Load index from file if available
   - Simple, high benefit

2. **Phase 2** (Optional): Read coordination
   - Coordinate reads to maximize cache hits
   - More complex, moderate benefit

## Final Recommendation

### ✅ **Hybrid Approach: Shared Index + OS Cache**

**Why**:
- ✅ Best balance of simplicity and benefit
- ✅ Works with current architecture
- ✅ Shared index provides significant benefit
- ✅ OS cache already provides "single read" effect
- ✅ No memory issues with large files
- ✅ Network storage compatible

**Implementation**:
1. Add index file save/load to `VideoFileInput`
2. Audio player loads index if available
3. Optional: Add read coordination for cache optimization

**Expected Benefits**:
- **Index Sharing**: Audio player gets optimized seeking
- **Cache Optimization**: Second process reads from OS cache
- **Timecode Sync**: Both processes read same regions → optimal cache usage

This approach provides most benefits of "single read" without the complexity of true single-read solutions.

