set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Pin deps to the VS2022 14.44 toolset: fmt 9.1.0 (CommonLib baseline) uses
# stdext::checked_array_iterator, removed from the 14.5x MSVC STL.
set(VCPKG_PLATFORM_TOOLSET_VERSION "14.44")
