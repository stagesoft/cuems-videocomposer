# Shared Library Approach: File Reading Evaluation

## Question

If `cuems-mediadecoder` were changed from a **static library (submodule)** to a **shared library (.so)**, would we benefit from reading files only once when both `cuems-audioplayer` and `cuems-videocomposer` access the same file?

## Current Architecture

### Static Library (Current)
- Each process (`cuems-videocomposer`, `cuems-audioplayer`) links `cuems-mediadecoder` statically
- Each process has its own copy of the library code in memory
- Each process has its own file descriptors
- Each process opens files independently via `avformat_open_input()`

### Shared Library (Proposed)
- Both processes load the same `.so` file
- Library code is shared in physical memory (OS handles this)
- **But**: Each process still has its own memory space
- Each process still has its own file descriptors
- Each process still opens files independently

## Key Question: Can Shared Library Enable Single File Read?

### ❌ **Short Answer: No**

**Why shared library doesn't help**:
1. **Process Isolation**: Each process has its own virtual memory space
   - Even with shared library, processes are isolated
   - File descriptors are per-process
   - Memory buffers are per-process

2. **FFmpeg Behavior**: `avformat_open_input()` opens files per-process
   - Each process calls `avformat_open_input()` independently
   - Each process gets its own `AVFormatContext`
   - Each process reads from its own file descriptor

3. **No Automatic Sharing**: Shared library doesn't automatically share:
   - File descriptors
   - File buffers
   - Decoded frames
   - Index data

### ✅ **What Actually Helps: OS File Cache**

**Current situation (already working)**:
- OS caches file data in physical memory
- Both processes benefit from the same cached data
- **This already happens with static library!**
- Shared library doesn't improve this

**Example**:
```
Process A reads file → OS caches data in RAM
Process B reads same file → OS serves from cache (no disk read)
→ Works with both static and shared library
```

## Detailed Analysis

### Scenario 1: File Opening

**Static Library (Current)**:
```
cuems-videocomposer: avformat_open_input() → file descriptor A
cuems-audioplayer:   avformat_open_input() → file descriptor B
→ Two file descriptors, but OS cache helps both
```

**Shared Library (Proposed)**:
```
cuems-videocomposer: avformat_open_input() → file descriptor A
cuems-audioplayer:   avformat_open_input() → file descriptor B
→ Still two file descriptors, same as static library
```

**Result**: ✅ **No difference** - Both approaches have same behavior

### Scenario 2: File Reading

**Static Library (Current)**:
```
cuems-videocomposer: av_read_frame() → reads from file descriptor A
cuems-audioplayer:   av_read_frame() → reads from file descriptor B
→ OS cache serves both from same physical memory
```

**Shared Library (Proposed)**:
```
cuems-videocomposer: av_read_frame() → reads from file descriptor A
cuems-audioplayer:   av_read_frame() → reads from file descriptor B
→ Same behavior, OS cache still helps
```

**Result**: ✅ **No difference** - OS cache works the same way

### Scenario 3: Index Data Sharing

**Static Library (Current)**:
```
cuems-videocomposer: Builds index in process memory
cuems-audioplayer:   No index (or builds its own)
→ Index data not shared
```

**Shared Library (Proposed)**:
```
cuems-videocomposer: Builds index in process memory
cuems-audioplayer:   No index (or builds its own)
→ Still not shared (different memory spaces)
```

**Result**: ❌ **No improvement** - Index data still not shared

**To share index data, you would need**:
- Shared memory (`shm_open`, `mmap` with `MAP_SHARED`)
- IPC mechanisms (sockets, pipes, message queues)
- File-based index storage
- **Not just a shared library**

## What Could Actually Help

### Option 1: Memory-Mapped Files (mmap)

**Concept**: Map file into virtual memory, share physical pages

**Implementation**:
```c
// Open file
int fd = open("video.mp4", O_RDONLY);

// Memory map file
void* mapped = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);

// Both processes can access same physical memory
```

**Benefits**:
- ✅ Both processes share same physical memory pages
- ✅ Only one copy of file data in RAM
- ✅ OS handles sharing automatically

**Limitations**:
- ⚠️ FFmpeg doesn't use mmap by default
- ⚠️ Would need custom I/O context (`AVIOContext`)
- ⚠️ Complex to implement
- ⚠️ May not work well with seeking

**Verdict**: ⚠️ **Possible but complex** - Requires significant FFmpeg customization

### Option 2: Shared Memory for Index

**Concept**: Store index in shared memory segment

**Implementation**:
```c
// Create shared memory segment
int shm_fd = shm_open("/cuems-video-index", O_CREAT | O_RDWR, 0666);
ftruncate(shm_fd, index_size);
void* shared_index = mmap(NULL, index_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

// cuems-videocomposer: Writes index here
// cuems-audioplayer: Reads index from here
```

**Benefits**:
- ✅ Index data shared between processes
- ✅ Audio player can use video player's index
- ✅ Reduces memory usage

**Limitations**:
- ⚠️ Requires IPC coordination
- ⚠️ Need to handle synchronization
- ⚠️ Process lifecycle management (cleanup)

**Verdict**: ✅ **Feasible** - Would help with index sharing, but separate from shared library

### Option 3: File-Based Index Cache

**Concept**: Write index to disk, both processes read it

**Implementation**:
```c
// cuems-videocomposer: After indexing
save_index_to_file("video.mp4.idx", frameIndex_, frameCount_);

// cuems-audioplayer: Before playback
if (index_file_exists("video.mp4.idx")) {
    load_index_from_file("video.mp4.idx", &frameIndex_, &frameCount_);
}
```

**Benefits**:
- ✅ Index shared via file system
- ✅ Simple to implement
- ✅ Works with both static and shared library

**Limitations**:
- ⚠️ Disk I/O for index file
- ⚠️ Need to handle index file updates
- ⚠️ File locking for concurrent access

**Verdict**: ✅ **Feasible** - Simple and effective, works with current architecture

## Comparison: Static vs Shared Library

| Aspect | Static Library (Current) | Shared Library | Difference |
|--------|-------------------------|----------------|------------|
| **File Descriptors** | One per process | One per process | ❌ None |
| **File Reading** | Per-process, OS cached | Per-process, OS cached | ❌ None |
| **Memory Usage (Code)** | Duplicated in each process | Shared physical memory | ✅ Slight benefit |
| **Memory Usage (Data)** | Separate buffers per process | Separate buffers per process | ❌ None |
| **Index Sharing** | Not shared | Not shared | ❌ None |
| **OS File Cache** | Works | Works | ❌ None |
| **Implementation Complexity** | Simple (submodule) | Medium (build system) | ⚠️ More complex |

## Real-World Impact

### Memory Savings (Code Only)

**Static Library**:
- `cuems-mediadecoder` code: ~500 KB per process
- 2 processes = ~1 MB total
- Code duplicated in memory

**Shared Library**:
- `cuems-mediadecoder` code: ~500 KB (shared)
- 2 processes = ~500 KB total
- Code shared in physical memory

**Savings**: ~500 KB RAM (negligible on modern systems)

### File Reading (No Change)

**Both approaches**:
- Each process reads file independently
- OS cache benefits both
- No reduction in disk I/O
- No reduction in memory for file data

### Index Sharing (No Change)

**Both approaches**:
- Index data not shared
- Each process has its own index (if any)
- Would need separate mechanism (shared memory, file-based)

## Recommendation

### ❌ **Shared Library Won't Help with File Reading**

**Reasons**:
1. **Process Isolation**: Shared library doesn't break process boundaries
2. **File Descriptors**: Still per-process, regardless of library type
3. **OS Cache**: Already works optimally with static library
4. **Minimal Benefit**: Only saves code memory (~500 KB), not data memory

### ✅ **What Would Actually Help**

1. **Memory-Mapped Files** (mmap):
   - Share physical memory pages
   - Requires FFmpeg customization
   - Complex but effective

2. **Shared Memory for Index**:
   - Share index data between processes
   - Requires IPC coordination
   - Moderate complexity

3. **File-Based Index Cache**:
   - Share index via file system
   - Simple to implement
   - Works with current architecture

4. **OS File Cache** (Already Working):
   - Both processes benefit from cached data
   - No changes needed
   - Already optimal

## Conclusion

### ❌ **Shared Library Doesn't Enable Single File Read**

**Key Points**:
- Shared library shares **code**, not **data**
- File descriptors are still per-process
- File reading is still per-process
- OS cache already optimizes this

### ✅ **Current Approach is Already Optimal**

**From TIMECODE_SYNC_CONCURRENT_ACCESS.md**:
- OS file cache benefits both processes
- Synchronized access is actually beneficial
- Sequential reads are efficient
- No conflicts or correctness issues

### 🔧 **If You Want to Share File Data**

**Options** (in order of complexity):
1. **File-based index cache** (simplest, recommended)
2. **Shared memory for index** (moderate complexity)
3. **Memory-mapped files** (most complex, requires FFmpeg changes)

**None of these require shared library** - they work with static library too.

### Final Verdict

**❌ Don't switch to shared library for file sharing**

**Reasons**:
- Won't enable single file read
- Won't share file data
- Won't share index data
- Minimal memory savings (code only)
- More complex build system
- Current approach is already optimal

**✅ Keep static library approach**

**Benefits**:
- Simpler build system (submodule)
- No runtime dependencies
- Easier deployment
- OS cache already optimizes file access
- Can add file-based index sharing if needed

## Alternative: Hybrid Approach

If you want to share index data, consider:

**Static library + File-based index**:
- Keep static library (simple)
- Add index file writing/reading
- Both processes can use same index file
- Simple to implement
- No shared library needed

This gives you index sharing without the complexity of shared library or shared memory.

