set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Pin to VS 2022 toolset to avoid ABI mismatch with Build Tools 18
set(VCPKG_PLATFORM_TOOLSET v143)
