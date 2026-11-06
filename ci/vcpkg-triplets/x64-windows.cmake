# KiCadD CI: upstream's x64-windows (dynamic CRT + DLLs), release only, which halves the
# dependency build. Same name as the stock triplet so KiCad's CMake finds the usual paths.
set( VCPKG_TARGET_ARCHITECTURE x64 )
set( VCPKG_CRT_LINKAGE dynamic )
set( VCPKG_LIBRARY_LINKAGE dynamic )
set( VCPKG_BUILD_TYPE release )
set( VCPKG_DISABLE_COMPILER_TRACKING ON )
