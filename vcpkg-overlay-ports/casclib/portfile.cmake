vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ladislav-zezula/CascLib
    REF 1623348517352ff0d6364d47533c0d7f118b46e1
    SHA512 1b6dd77a399ff21a278d1ac68efa1417f2a9472b241401d3f6cb8f01ea702b32b4296012bb71ae62a932f9b0fa2b6d3035befe6f3ba0d567e85ac747c503b07c
    HEAD_REF master
    PATCHES
        # CascDecompress (every zlib-compressed CASC frame) through libdeflate, 1.7 times
        # zlib's inflate on BO4 export data (340 -> 580 MB/s out, 64 KB frames); zlib stays as the fallback for what libdeflate
        # refuses (output longer than the frame, truncated stream), so results are unchanged.
        libdeflate-decompress.patch
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" CASC_BUILD_SHARED_LIB)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" CASC_BUILD_STATIC_LIB)

# ON, unlike the upstream port, which builds ASCII and says so.
#
# This project compiles with _UNICODE, so CascLib's headers declare its path arguments as
# const wchar_t*. The ASCII library reads them as const char*, and because CascLib.h wraps
# everything in extern "C" the two link together without a word of warning - the only symptom is
# every CASC storage failing to open, since a path stops at its first zero byte. That cost a long
# investigation: Black Ops 4 loaded fine and exported two images out of forty.
#
# The upstream portfile points at exactly this overlay as the way to switch.
set(CASC_UNICODE ON)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_REQUIRE_FIND_PACKAGE_ZLIB=ON
        -DCASC_BUILD_SHARED_LIB=${CASC_BUILD_SHARED_LIB}
        -DCASC_BUILD_STATIC_LIB=${CASC_BUILD_STATIC_LIB}
        -DCASC_UNICODE=${CASC_UNICODE}
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CascLib)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
