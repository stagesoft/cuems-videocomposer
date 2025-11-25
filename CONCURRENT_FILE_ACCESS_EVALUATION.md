# Concurrent File Access Evaluation: cuems-audioplayer + cuems-videocomposer

## Scenario

Both `cuems-audioplayer` and `cuems-videocomposer` reading and playing the same media file simultaneously:
- **cuems-videocomposer**: Reading video stream, possibly indexing
- **cuems-audioplayer**: Reading audio stream from the same file
- **Potential conflicts**: File access, seeking, indexing, I/O contention

## Analysis

### ✅ **Generally Safe: Reading Operations**

#### File System Level
- **Unix/Linux**: Multiple processes can read the same file simultaneously
  - Each process gets its own file descriptor
  - Each process maintains its own file position (seek position)
  - No file-level locking required for read operations
  - OS handles concurrent reads efficiently

#### FFmpeg Behavior
- **`avformat_open_input()`**: Opens file in read-only mode
  - No exclusive locking
  - Multiple processes can open the same file
  - Each `AVFormatContext` is independent

- **`av_read_frame()`**: Reads packets independently
  - Each process maintains its own read position
  - No interference between processes
  - File position changes in one process don't affect others

- **`av_seek_frame()`**: Seeks independently
  - Each process seeks its own file descriptor
  - No cross-process interference

### ⚠️ **Potential Issues**

#### 1. I/O Contention (Performance Impact)

**Problem**: Both applications reading from the same file simultaneously
- **Disk I/O**: Both processes competing for disk bandwidth
- **Cache**: OS file cache benefits both, but may be split
- **Network**: If file is on network share, bandwidth contention

**Impact**:
- **Sequential playback**: Usually fine (OS cache helps)
- **Random seeking**: May cause disk thrashing
- **Background indexing**: High I/O load, may impact playback

**Severity**: ⚠️ **Medium** - Performance degradation, not correctness issue

**Mitigation**:
- OS file cache helps significantly
- Modern SSDs handle concurrent reads well
- Network storage may be slower but still works

#### 2. Background Indexing Conflicts

**Problem**: If `cuems-videocomposer` is doing background indexing while `cuems-audioplayer` is playing

**Scenario**:
```
cuems-videocomposer: Background thread reading entire file (indexing)
cuems-audioplayer: Reading audio packets sequentially (playback)
```

**Impact**:
- **I/O contention**: Background indexing reads entire file, competing with audio playback
- **Disk head movement**: Random seeks during indexing vs sequential audio reads
- **Performance**: Audio playback may stutter during indexing

**Severity**: ⚠️ **Medium-High** - Can cause audio dropouts

**Mitigation**:
- Background indexing should be low-priority (already planned)
- Pause indexing during active playback (if detected)
- Use separate file handles (already the case)

#### 3. Seeking Conflicts (Not an Issue)

**Problem**: One app seeks while the other is reading

**Reality**: ✅ **Not a problem**
- Each process has its own file descriptor
- Seeking in one process doesn't affect the other
- File position is per-process, not global

**Example**:
```
Process A (videocomposer): seeks to frame 1000
Process B (audioplayer): continues reading from frame 500
→ No conflict, independent file positions
```

#### 4. Index File Conflicts (If Implemented)

**Problem**: If both apps create index files (e.g., `.idx` files)

**Current Status**: ✅ **Not applicable**
- No index files are currently written to disk
- Indexing is in-memory only
- No file conflicts

**Future Consideration**: If index files are added:
- Use process-specific names (e.g., `.idx.pid`)
- Or use advisory locking (`flock`) for index files

#### 5. Network Streams (Different Behavior)

**Problem**: If file is a network stream (RTSP, HTTP)

**Reality**: ⚠️ **May have issues**
- Some network protocols don't support multiple connections
- RTSP: May require separate sessions
- HTTP: Usually fine (multiple connections allowed)
- UDP: Usually fine (stateless)

**Mitigation**:
- Each process opens its own connection
- Network protocols handle this at the protocol level
- May need separate session IDs for RTSP

### ✅ **What Works Well**

#### 1. Independent Stream Access
- **Video stream**: `cuems-videocomposer` reads video packets
- **Audio stream**: `cuems-audioplayer` reads audio packets
- **No conflict**: Different stream indices, independent access

#### 2. Separate Codec Contexts
- Each process has its own `AVCodecContext`
- No shared state between processes
- Decoding happens independently

#### 3. OS File Cache
- First process reads file → OS caches it
- Second process benefits from cache
- Reduces actual disk I/O

#### 4. Read-Only Access
- Both processes open file read-only
- No write conflicts
- No file corruption risk

## Real-World Testing Scenarios

### Scenario 1: Sequential Playback
```
cuems-videocomposer: Playing video, reading sequentially
cuems-audioplayer: Playing audio, reading sequentially
```
**Result**: ✅ **Works well**
- Sequential reads are efficient
- OS cache helps both processes
- Minimal I/O contention

### Scenario 2: Video Seeking + Audio Playback
```
cuems-videocomposer: User scrubbing video (random seeks)
cuems-audioplayer: Playing audio continuously
```
**Result**: ⚠️ **Works but may impact performance**
- Video seeks cause random disk access
- Audio sequential reads may be interrupted
- May cause audio dropouts on slow storage

### Scenario 3: Background Indexing + Audio Playback
```
cuems-videocomposer: Background thread indexing (reading entire file)
cuems-audioplayer: Playing audio
```
**Result**: ⚠️ **Performance impact**
- Background indexing reads entire file
- High I/O load competes with audio playback
- Audio may stutter during indexing

**Mitigation**: 
- Background indexing should be low-priority (already planned)
- Consider pausing indexing if other process detected (complex)

### Scenario 4: Both Apps Indexing
```
cuems-videocomposer: Indexing video
cuems-audioplayer: Indexing audio (if implemented)
```
**Result**: ⚠️ **High I/O load**
- Both reading entire file simultaneously
- Maximum disk contention
- May impact system performance

**Mitigation**:
- Indexing is optional (can be disabled)
- User can disable indexing in one app

## Technical Details

### File Descriptor Independence

```cpp
// Process A (videocomposer)
int fd1 = open("video.mp4", O_RDONLY);
lseek(fd1, 1000000, SEEK_SET);  // Seek to position 1MB
read(fd1, buffer, 1024);         // Reads from 1MB

// Process B (audioplayer)
int fd2 = open("video.mp4", O_RDONLY);
lseek(fd2, 500000, SEEK_SET);    // Seek to position 500KB
read(fd2, buffer, 1024);         // Reads from 500KB

// No conflict - independent file descriptors
```

### FFmpeg Context Independence

```cpp
// Process A
AVFormatContext* fmtCtx1;
avformat_open_input(&fmtCtx1, "video.mp4", ...);
av_seek_frame(fmtCtx1, ...);  // Seeks in Process A's context

// Process B
AVFormatContext* fmtCtx2;
avformat_open_input(&fmtCtx2, "video.mp4", ...);
av_seek_frame(fmtCtx2, ...);  // Seeks in Process B's context

// No conflict - independent contexts
```

### Memory Mapping (If Used)

**Note**: FFmpeg doesn't use memory mapping by default for most formats, but if it did:
- Multiple processes can memory-map the same file
- OS handles this efficiently
- Read-only mappings are safe

## Recommendations

### ✅ **Current Implementation: Safe**

The current implementation is **safe** for concurrent access:
- ✅ Read-only file access
- ✅ Independent file descriptors
- ✅ No shared state between processes
- ✅ No file locking needed

### ⚠️ **Performance Considerations**

1. **Background Indexing**:
   - Use low-priority thread (already planned)
   - Consider detecting other processes (optional, complex)
   - User can disable indexing if needed

2. **I/O Optimization**:
   - OS file cache helps significantly
   - Sequential reads are more efficient than random
   - Consider SSD for better concurrent performance

3. **User Control**:
   - Allow disabling indexing: `--no-index`
   - Allow disabling background indexing: `--disable-background-indexing`
   - User can choose based on their setup

### 🔧 **Optional Enhancements**

#### 1. Process Detection (Advanced)
```cpp
// Check if another process is accessing the same file
bool isFileInUse(const std::string& path) {
    // Use lsof or /proc to check
    // Complex and platform-specific
    // Probably not worth the effort
}
```

**Verdict**: ❌ **Not recommended** - Too complex, minimal benefit

#### 2. Advisory File Locking (Optional)
```cpp
// Use advisory locking (doesn't prevent access, just signals intent)
int fd = open("video.mp4", O_RDONLY);
flock(fd, LOCK_SH);  // Shared lock (allows other readers)
```

**Verdict**: ⚠️ **Optional** - Doesn't prevent access, just signals intent
- Could be useful for coordination
- But adds complexity
- Not necessary for correctness

#### 3. Index File Coordination (If Index Files Added)
```cpp
// If index files are written to disk:
std::string indexFile = path + ".idx." + std::to_string(getpid());
// Process-specific index files
```

**Verdict**: ✅ **Recommended** - If index files are added in future

## Conclusion

### ✅ **Safe for Concurrent Access**

**Summary**:
- ✅ **Correctness**: No issues - both apps can read the same file safely
- ⚠️ **Performance**: Some I/O contention possible, but usually acceptable
- ✅ **No file corruption risk**: Read-only access prevents corruption
- ✅ **No locking needed**: File system handles concurrent reads

### Key Points:

1. **File System**: Unix/Linux allows multiple processes to read the same file
2. **FFmpeg**: Each process has independent `AVFormatContext`
3. **File Descriptors**: Each process has its own file position
4. **I/O Contention**: Possible but usually acceptable (OS cache helps)
5. **Background Indexing**: May cause performance impact, but low-priority helps

### Recommendation:

**✅ Proceed with current implementation**

The concurrent access scenario is **safe and supported**. The main consideration is **performance**, not correctness:
- I/O contention may occur but is usually acceptable
- OS file cache helps significantly
- Background indexing should be low-priority (already planned)
- User can disable indexing if needed

**No changes required** to support concurrent access - it works out of the box.

### Testing Recommendations:

1. **Test concurrent playback**: Both apps playing same file
2. **Test seeking**: One app seeking while other plays
3. **Test background indexing**: Video indexing while audio plays
4. **Test on different storage**: Local disk, network share, SSD vs HDD
5. **Monitor performance**: CPU, I/O, memory usage

### Future Considerations:

- If index files are added: Use process-specific names
- If write operations are added: Implement proper locking
- If network streams: Ensure protocol supports multiple connections

