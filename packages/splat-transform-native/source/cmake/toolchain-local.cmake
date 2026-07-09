# Minimal toolchain: bypass vcpkg, find packages from known paths
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

# Package install root
set(VCPKG_INSTALL_DIR "D:/code/vcpkg/installed/x64-windows-static")

# Package config directories
set(Eigen3_DIR "${VCPKG_INSTALL_DIR}/share/eigen3")
set(nanoflann_DIR "${VCPKG_INSTALL_DIR}/share/nanoflann")
set(WebP_DIR "${VCPKG_INSTALL_DIR}/share/WebP")
set(libavif_DIR "${VCPKG_INSTALL_DIR}/share/libavif")
set(libjpeg-turbo_DIR "${VCPKG_INSTALL_DIR}/share/libjpeg-turbo")
set(libyuv_DIR "${VCPKG_INSTALL_DIR}/share/libyuv")

# JPEG (for libyuv dependency)
set(JPEG_INCLUDE_DIR "${VCPKG_INSTALL_DIR}/include")
set(JPEG_LIBRARY "${VCPKG_INSTALL_DIR}/lib/jpeg.lib")
set(Threads_DIR "")

# cmake-js config from project cmake directory
set(cmake-js_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/share/cmake-js")

# Include directories for header-only libs
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Node.js N-API setup
set(NODE_API_INC "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-api-headers/include")
set(CMAKE_JS_INC "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api;${NODE_API_INC}")
