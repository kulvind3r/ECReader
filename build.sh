#!/bin/bash
# Build script for EC Register Reader using MinGW in WSL
# This cross-compiles a Windows .exe from Linux

set -e  # Exit on error

echo "========================================================"
echo "  EC Register Reader - WSL Build Script"
echo "  Cross-compiling for Windows using MinGW"
echo "========================================================"
echo ""

# Check if MinGW is installed
if ! command -v x86_64-w64-mingw32-g++ &> /dev/null; then
    echo "❌ MinGW-w64 not found!"
    echo ""
    echo "Install it with:"
    echo "  sudo apt update"
    echo "  sudo apt install mingw-w64"
    echo ""
    exit 1
fi

echo "✓ MinGW-w64 found"
echo ""

# Determine which source file to build
if [ -f "ECReader.cpp" ]; then
    SOURCE="ECReader.cpp"
    OUTPUT="./build/ECReader.exe"
    echo "Building: Source file found"
else
    echo "❌ No source file found!"
    echo "Expected: ECReader.cpp"
    exit 1
fi

echo "Source:   $SOURCE"
echo "Output:   $OUTPUT"
echo ""

# Build
echo "Compiling..."

# Create Build directory
mkdir -p build

# Extract version from source
VERSION=$(grep -oP '#define ECREADER_VERSION "\K[^"]+' "$SOURCE")
if [ -z "$VERSION" ]; then
    VERSION=$(date +"%Y.%m.%d")
fi

# Parse version into components
IFS='.' read -r VER_MAJOR VER_MINOR VER_PATCH <<< "$VERSION"
VER_MAJOR=${VER_MAJOR:-1}
VER_MINOR=${VER_MINOR:-0}
VER_PATCH=${VER_PATCH:-0}

# Generate version resource file
cat > version.rc << EOF
#include <windows.h>

VS_VERSION_INFO VERSIONINFO
FILEVERSION    ${VER_MAJOR},${VER_MINOR},${VER_PATCH},0
PRODUCTVERSION ${VER_MAJOR},${VER_MINOR},${VER_PATCH},0
FILEFLAGSMASK  VS_FFI_FILEFLAGSMASK
FILEFLAGS      0x0L
FILEOS         VOS_NT_WINDOWS32
FILETYPE       VFT_APP
FILESUBTYPE    VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName",      "ECReader Project"
            VALUE "FileDescription",  "Embedded Controller Register Reader"
            VALUE "FileVersion",      "${VERSION}"
            VALUE "InternalName",     "ECReader"
            VALUE "LegalCopyright",   "Educational/Utility - Use at your own risk"
            VALUE "OriginalFilename", "ECReader.exe"
            VALUE "ProductName",      "ECReader"
            VALUE "ProductVersion",   "${VERSION}"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOF

# Compile version resource
echo "Embedding version ${VERSION}..."
x86_64-w64-mingw32-windres version.rc -O coff -o version.o
RESOURCE_OBJ="version.o"

# Compile additional resource file if it exists (for icon)
if [ -f "resource.rc" ]; then
    echo "Compiling additional resources..."
    x86_64-w64-mingw32-windres resource.rc -O coff -o resource.o
    RESOURCE_OBJ="$RESOURCE_OBJ resource.o"
fi

x86_64-w64-mingw32-g++ \
    -o "$OUTPUT" \
    "$SOURCE" \
    $RESOURCE_OBJ \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -O2 \
    -Wall \
    -Wextra \
    -fdiagnostics-plain-output

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================================"
    echo "  ✓ Build Successful!"
    echo "========================================================"
    echo ""

    # Get file size
    SIZE=$(ls -lh "$OUTPUT" | awk '{print $5}')
    echo "Output file: $OUTPUT"
    echo "Size:        $SIZE"
    echo ""

    echo "Version:     $VERSION"
    echo ""

    # Create release zip
    ZIP_NAME="ECReader-v${VERSION}.zip"
    echo "Creating release package: $ZIP_NAME"

    # Check if zip is available
    if command -v zip &> /dev/null; then
        # Remove old zip if exists
        rm -f "./build/$ZIP_NAME"

        # Copy module for quick testing
        cp -f LpcACPIEC.bin ./build/

        # Create zip with exe and dependencies
        zip -q -j "./build/$ZIP_NAME" "$OUTPUT" LpcACPIEC.bin

        if [ $? -eq 0 ]; then
            ZIP_SIZE=$(ls -lh "./build/$ZIP_NAME" | awk '{print $5}')
            echo "✓ Release package created: ./build/$ZIP_NAME ($ZIP_SIZE)"
        else
            echo "⚠ Warning: Failed to create release package"
        fi
    else
        echo "⚠ zip not found, skipping release package"
        echo "  Install with: sudo apt install zip"
    fi
    echo ""
else
    echo ""
    echo "❌ Build failed!"
    exit 1
fi
