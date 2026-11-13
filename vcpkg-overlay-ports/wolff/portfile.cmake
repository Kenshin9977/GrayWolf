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
        REF 30a9be435dc6eb53ad0d2658bc0776ce4b4bdda4
        SHA512 dda64281b8011e7c194728a55ccbf6f49572e792ac58097a77203cfffcf260eab4bdd295a9b87d67b546605b067effe47ba438edf6a3c8364faf523080eedb0a
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
