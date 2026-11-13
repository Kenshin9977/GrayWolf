# Wolff, from its published repository at a pinned commit.
#
# To build from a local checkout instead (working on Wolff itself), set WOLFF_SOURCE_DIR (vcpkg
# hides the environment from ports: pass it with VCPKG_KEEP_ENV_VARS=WOLFF_SOURCE_DIR). vcpkg does
# not hash a local source into the package's ABI: after changing Wolff, reinstall it
#   vcpkg remove wolff --triplet x64-windows-static-md
#   vcpkg install wolff --triplet x64-windows-static-md --overlay-ports=vcpkg-overlay-ports --no-binarycaching
# (it depends on this folder's CascLib overlay, which --overlay-ports picks up too).

if(DEFINED ENV{WOLFF_SOURCE_DIR})
    file(TO_CMAKE_PATH "$ENV{WOLFF_SOURCE_DIR}" SOURCE_PATH)
    if(NOT EXISTS "${SOURCE_PATH}/CMakeLists.txt")
        message(FATAL_ERROR "Wolff's source is not at ${SOURCE_PATH} (WOLFF_SOURCE_DIR)")
    endif()
    message(STATUS "Wolff from ${SOURCE_PATH}")
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO Kenshin9977/wolff
        REF 52b923ac6af2cdd5b8be5b492888e2ea310b74ec
        SHA512 c43b8b391f8d89fffa870f45d971b90c1141c5245c5ed6d40a8b031e10aeaaec871d387ad59e8a4042167bd4536f7398c8a02e4b675cb947278c4e0d636c53e6
        HEAD_REF main
    )
endif()

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# The walker finds the profiles this port installs (share/wolff/profiles) in the installed tree
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DWOLFF_BUILD_TOOLS=OFF
    "-DWOLFF_INSTALLED_PROFILES=${CURRENT_INSTALLED_DIR}/share/${PORT}/profiles")
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH share/wolff)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
