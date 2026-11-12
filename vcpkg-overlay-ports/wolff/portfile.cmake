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
        REF dc407ea9116cd114051b4a92833d66f2a329dc12
        SHA512 3c228b9a8d7f9c1a085d3056887f3691f3893b7042240b7c33e27c10a8b970851aa3fdb6c9bb165df6ef28e07935e3fc9663fca21fc892412543b7abf25c406f
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
