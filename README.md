# MotionCam Fuse (Fuse-2026)

Desktop tool for mounting MotionCam recordings, exporting DNGs, and reviewing clips.

## Build

Windows (Visual Studio + Qt + vcpkg):
```
git submodule update --init --recursive
.\build.bat
```
Executable lands in `build\Release\MotionCamFuse.exe`.

macOS (Qt + vcpkg, CMake):
```
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

macOS x86_64 (Intel or Hackintosh):
```
git submodule update --init --recursive
cmake -S . -B build-x86 -DCMAKE_BUILD_TYPE=Release -DMOTIONCAMFUSE_MACOS_X86_64=ON -DVCPKG_TARGET_TRIPLET=x64-osx
cmake --build build-x86 --config Release
```

Universal macOS (x86_64 + arm64) can be built by adding `-DMOTIONCAMFUSE_MACOS_UNIVERSAL=ON` when your dependencies are available as universal binaries.

## Docs

See `HELP.md` for usage, preferences, and feature details.
