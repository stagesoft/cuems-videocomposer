# HAP SDK Backup Copy

This directory contains a backup copy of the Vidvox HAP SDK source files.

## Purpose

This backup ensures the build works even if:
- Git submodules are not initialized (`git submodule update --init`)
- The submodule directory is missing
- Building from a source archive that doesn't include submodules

## Source

The files in this directory are from the Vidvox HAP SDK:
- **Repository**: https://github.com/Vidvox/hap
- **License**: See LICENSE file in the git submodule

## Build System Behavior

CMake will use sources in this order:

1. **Primary**: `external/hap/source/` (git submodule)
   - Used when submodule is initialized
   - Points to specific commit for reproducible builds

2. **Fallback**: `external/hap_backup/source/` (this directory)
   - Used when submodule is not available
   - Ensures build works without submodule initialization

## Updating the Backup

When updating the HAP submodule to a new version, also update this backup:

```bash
# Update submodule
cd external/hap
git pull origin master
cd ../..

# Copy updated files to backup
cp external/hap/source/hap.c external/hap_backup/source/
cp external/hap/source/hap.h external/hap_backup/source/

# Commit both changes
git add external/hap external/hap_backup
git commit -m "Update HAP SDK to latest version"
```

## Files

- `source/hap.c` - HAP decoder/encoder implementation
- `source/hap.h` - HAP API header

## Notes

- This backup should match the version in the git submodule
- Keep both in sync when updating HAP SDK versions
- The backup is committed to the repository for convenience

