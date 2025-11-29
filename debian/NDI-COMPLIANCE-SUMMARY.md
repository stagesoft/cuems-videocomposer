# NDI SDK License Compliance Summary

This document summarizes how CUEMS Video Composer complies with the NDI SDK License Agreement when distributed as a Debian package.

## ✅ Compliance Checklist

### 1. Library Installation Location ✅
- **Requirement**: NDI libraries must be installed to application directories, NOT system paths
- **Implementation**: Libraries installed to `/usr/lib/cuems-videocomposer/`
- **File**: `debian/rules` - installs NDI .so files to private directory

### 2. EULA Requirements ✅
- **Requirement**: Application EULA must include all NDI SDK license terms
- **Implementation**: Complete EULA in `debian/ndi-compliance/EULA`
- **Includes**:
  - Prohibition on modifying NDI SDK
  - Prohibition on reverse engineering/disassembly
  - Warranty disclaimers on behalf of NDI
  - Liability disclaimers for NDI
  - Export compliance requirements
  - Copyright notice (NewTek, Inc.)

### 3. Trademark Attribution ✅
- **Requirement**: Use "NDI®" with ® symbol and attribution statement
- **Implementation**: 
  - `debian/copyright` - includes trademark attribution
  - `debian/README.Debian` - includes trademark attribution
  - `debian/ndi-compliance/README` - includes trademark attribution

### 4. Website Link ✅
- **Requirement**: Include link to https://ndi.video/ near all NDI usage
- **Implementation**:
  - `debian/README.Debian` - includes link
  - `debian/ndi-compliance/README` - includes link
  - `debian/ndi-compliance/EULA` - includes link

### 5. Copyright Notices ✅
- **Requirement**: Include NDI copyright notices
- **Implementation**: 
  - `debian/copyright` - includes NDI copyright section
  - `debian/ndi-compliance/EULA` - includes copyright notice

### 6. No NDI Tools Distribution ✅
- **Requirement**: Do not distribute NDI Tools, provide link instead
- **Implementation**: 
  - `debian/ndi-compliance/README` - includes link to https://ndi.video/tools
  - No NDI tools bundled in package

### 7. Runtime Library Path ✅
- **Requirement**: Application must find NDI libraries at runtime
- **Implementation**: 
  - `CMakeLists.txt` - sets `INSTALL_RPATH` to `/usr/lib/cuems-videocomposer/`
  - Application can find NDI libraries without LD_LIBRARY_PATH

## Files Created/Modified

### Debian Packaging Files
- `debian/control` - Package metadata with NDI compliance note
- `debian/copyright` - Copyright file with NDI section
- `debian/rules` - Build rules with NDI library installation
- `debian/changelog` - Package changelog
- `debian/compat` - Debian compatibility level
- `debian/README.Debian` - Debian-specific documentation
- `debian/source/format` - Source package format

### NDI Compliance Files
- `debian/ndi-compliance/EULA` - Complete EULA with all required terms
- `debian/ndi-compliance/README` - NDI information and compliance details

### Code Changes
- `CMakeLists.txt` - Added rpath configuration for NDI libraries

## Building the Package

To build the Debian package:

```bash
# Ensure NDI SDK is available (one of):
# - Installed to /opt/NDI SDK for Linux/
# - NDI_SDK_DIR environment variable set

# Build the package
dpkg-buildpackage -us -uc

# Or with debuild (recommended)
debuild -us -uc
```

## Installation Paths

After installation, NDI-related files will be at:
- `/usr/lib/cuems-videocomposer/libndi.so*` - NDI SDK libraries
- `/usr/share/doc/cuems-videocomposer/NDI-COMPLIANCE/EULA` - NDI EULA
- `/usr/share/doc/cuems-videocomposer/NDI-COMPLIANCE/README` - NDI information
- `/usr/share/doc/cuems-videocomposer/copyright` - Copyright file with NDI section
- `/usr/share/doc/cuems-videocomposer/README.Debian` - Debian-specific docs

## Verification

After building and installing the package, verify compliance:

```bash
# Check NDI libraries are in private directory
ls -la /usr/lib/cuems-videocomposer/libndi.so*

# Check rpath is set correctly
readelf -d /usr/bin/cuems-videocomposer | grep RPATH

# Check documentation is installed
ls -la /usr/share/doc/cuems-videocomposer/NDI-COMPLIANCE/
```

## Legal Status

✅ **COMPLIANT**: This Debian package distribution complies with all requirements of the NDI SDK License Agreement.

The package:
- Bundles NDI SDK binaries in a private directory (not system paths)
- Includes complete EULA with all required terms
- Includes trademark attribution
- Includes website links
- Includes copyright notices
- Does not distribute NDI Tools (provides link instead)
- Sets proper rpath for runtime library discovery

