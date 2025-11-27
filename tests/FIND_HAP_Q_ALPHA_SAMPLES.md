# Finding HAP Q Alpha Test Files Online

This document lists potential sources for downloading HAP Q Alpha test files.

## Search Results Summary

**Finding**: Pre-existing HAP Q Alpha sample files are **not commonly available** online, but there are some sources:

✅ **VDMX Sample Packs** - Available for download (may contain HAP Q Alpha)  
❌ **GitHub Repositories** - No public repositories found with HAP Q Alpha samples  
❌ **Direct Downloads** - Very few public sources

## Potential Sources

### 1. Vidvox/VDMX Official Resources

**VDMX Blog** (https://vdmx.vidvox.net/blog/hap):
- Mentions free audio/visual sample clips in HAP format
- Available at 1080p and 480p
- **Note**: May not include HAP Q Alpha specifically, likely standard HAP

**VDMX Tutorial** (https://vdmx.vidvox.net/tutorials/working-with-hap-alpha-encoded-movies-in-vdmx):
- Tutorial on HAP Alpha movies
- May include sample clips
- **Note**: Focuses on HAP Alpha, not necessarily HAP Q Alpha

### 2. GitHub Repositories

Search for repositories containing HAP sample files:
```bash
# Search GitHub
https://github.com/search?q=hap+alpha+sample+files
https://github.com/search?q=hap_q_alpha+test
```

**Potential repositories to check**:
- Vidvox official repos: https://github.com/Vidvox
- HAP encoder plugins: https://github.com/disguise-one/hap-encoder-adobe-cc
- Community test file collections

### 3. Software Documentation

**Disguise Media Server**:
- Documentation mentions HAP Q Alpha support
- May have sample files in documentation or support resources
- Website: https://help.disguise.one

### 4. Community Forums

**VDMX Forums**: https://vdmx.vidvox.net/forums
- Community members may share test files
- Ask in relevant threads

**Video Production Forums**:
- Post-production communities
- VJ/live performance forums
- Members often share resources

### 5. Create Your Own

Since finding pre-made files is difficult, creating your own is often the most reliable option:

**Tools**:
1. **HAP Exporter for Adobe CC** (Free, Open Source)
   - GitHub: https://github.com/disguise-one/hap-encoder-adobe-cc
   - Works with Adobe Media Encoder

2. **AfterCodecs** (Commercial)
   - Professional plugin for Adobe CC
   - Website: https://www.autokroma.com/AfterCodecs/

3. **AVF Batch Exporter** (Free)
   - Batch conversion tool
   - Supports HAP Q Alpha

## Direct Download Sources

### ✅ VDMX Sample Packs (Confirmed Available)

**Vidvox/VDMX provides free HAP sample clips**:

1. **1080p Sample Pack**: http://vidvox.net/download/SamplePackOneHap1080p.zip
2. **480p Sample Pack**: http://vidvox.net/download/SamplePackOneHap480p.zip

**Source**: https://vdmx.vidvox.net/blog/hap

**Contents**: These packs contain sample clips in various HAP formats. They may include:
- HAP (standard)
- HAP Alpha
- HAP Q
- **Potentially HAP Q Alpha** (needs verification)

**Download Script**: Use `tests/download_hap_samples.sh` to automatically download and extract these packs.

```bash
./tests/download_hap_samples.sh
```

This will:
1. Download both sample packs (1080p and 480p)
2. Extract to `video_test_files/hap_samples/`
3. Check for HAP Q Alpha files
4. List all HAP files found

**Sample Pack Contents** (from 1080p pack):
- Fovi Mavuvy Drums Hap HD.mov (~57 MB)
- Fovi Mavuvy Lead Hap HD.mov (~50 MB)
- Gadane Fega Hap HD.mov (~193 MB)
- Hebopula Hap HD.mov (~30 MB)
- Kano Sopa Hap HD.mov (~54 MB)

**Note**: These files are from 2013 and may be standard HAP format. To verify if any are HAP Q Alpha:
1. Check codec tag/FourCC (HAP Q Alpha uses specific tags)
2. Test with FFmpeg decoder (should decode with `texture_count=2`)
3. Check file size (HAP Q Alpha files are typically larger)

### Check Vidvox GitHub

```bash
# Check Vidvox repositories for sample files
curl -s "https://api.github.com/users/Vidvox/repos" | grep -i "sample\|test\|example"
```

## Recommendation

**Best Approach**: Create your own test file using:
1. Source video with alpha channel (`test_with_alpha.mov` - already available)
2. HAP Exporter for Adobe CC or AfterCodecs
3. Export as HAP Q Alpha

**Why**:
- Guaranteed to have the exact format you need
- Can control resolution, duration, content
- No dependency on external sources
- Can be version-controlled with your test suite

## Alternative: Use Existing HAP Alpha

If you just need to test alpha channel support (not specifically HAP Q Alpha):
- `test_hap_alpha.mov` - Already created (HAP Alpha with DXT5 RGBA)
- Tests alpha channel functionality
- Only difference from HAP Q Alpha is color space (RGB vs YCoCg)

## Next Steps

1. **Check VDMX/Vidvox sites directly** for any sample file downloads
2. **Search GitHub** for community-shared test files
3. **Create your own** using available tools (most reliable)
4. **Ask in communities** if anyone can share a test file

## Script to Search for Files

```bash
#!/bin/bash
# Search for HAP Q Alpha files online

echo "Searching for HAP Q Alpha sample files..."

# Check VDMX blog
echo "Checking VDMX blog..."
curl -sL "https://vdmx.vidvox.net/blog/hap" | grep -i "download\|sample" | head -5

# Check hap.video
echo "Checking hap.video..."
curl -sL "https://hap.video" | grep -i "sample\|download" | head -5

# Search GitHub
echo "Searching GitHub..."
curl -s "https://api.github.com/search/repositories?q=hap+alpha+sample" | \
  python3 -c "import sys, json; [print(r['html_url']) for r in json.load(sys.stdin).get('items', [])[:5]]"
```

