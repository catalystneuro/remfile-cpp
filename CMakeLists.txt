cmake_minimum_required(VERSION 3.15)

project(
    remfile
    VERSION 0.1.1
    DESCRIPTION "Read-only HDF5 virtual file driver for reading remote files over HTTP(S)"
    LANGUAGES C CXX
)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(REMFILE_IS_TOP_LEVEL ON)
else()
  set(REMFILE_IS_TOP_LEVEL OFF)
endif()

option(REMFILE_BUILD_TESTS "Build the remfile test program" ${REMFILE_IS_TOP_LEVEL})
option(REMFILE_BUILD_PLUGIN "Build the dynamic HDF5 VFD plugin (requires HDF5 >= 1.14)" OFF)

find_package(HDF5 REQUIRED COMPONENTS C)
find_package(CURL REQUIRED)

# ---- Library ----

add_library(remfile src/remfile_vfd.cpp)
add_library(remfile::remfile ALIAS remfile)

target_compile_features(remfile PRIVATE cxx_std_17)

set_target_properties(remfile PROPERTIES
    EXPORT_NAME remfile
    OUTPUT_NAME remfile_vfd
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(remfile
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

# Include/link HDF5 explicitly via variables rather than imported targets for
# maximum portability across FindHDF5 implementations (see the note in the
# aqnwb top-level CMakeLists.txt).
target_include_directories(remfile SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${HDF5_INCLUDE_DIRS}>"
)
target_link_libraries(remfile
    PUBLIC ${HDF5_C_LIBRARIES}
    PRIVATE CURL::libcurl
)

# ---- Test program ----

if(REMFILE_BUILD_TESTS)
  add_executable(test_remfile tests/test_remfile.c)
  target_link_libraries(test_remfile PRIVATE remfile::remfile)
endif()

# ---- Dynamic VFD plugin ----

if(REMFILE_BUILD_PLUGIN)
  add_library(remfile_vfd_plugin MODULE src/remfile_vfd.cpp)
  target_compile_features(remfile_vfd_plugin PRIVATE cxx_std_17)
  target_compile_definitions(remfile_vfd_plugin PRIVATE REMFILE_VFD_BUILD_PLUGIN)
  target_include_directories(remfile_vfd_plugin
      PRIVATE "${PROJECT_SOURCE_DIR}/include"
  )
  target_include_directories(remfile_vfd_plugin SYSTEM PRIVATE ${HDF5_INCLUDE_DIRS})
  target_link_libraries(remfile_vfd_plugin
      PRIVATE ${HDF5_C_LIBRARIES} CURL::libcurl
  )
  set_target_properties(remfile_vfd_plugin PROPERTIES
      OUTPUT_NAME remfile_vfd_plugin
      PREFIX "lib"
  )
endif()

# ---- Install rules ----

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS remfile
    EXPORT remfileTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(REMFILE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/remfile")

install(EXPORT remfileTargets
    NAMESPACE remfile::
    DESTINATION "${REMFILE_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
    cmake/remfile-config.cmake.in
    "${PROJECT_BINARY_DIR}/remfile-config.cmake"
    INSTALL_DESTINATION "${REMFILE_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/remfile-config-version.cmake"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${PROJECT_BINARY_DIR}/remfile-config.cmake"
    "${PROJECT_BINARY_DIR}/remfile-config-version.cmake"
    DESTINATION "${REMFILE_INSTALL_CMAKEDIR}"
)

if(REMFILE_BUILD_PLUGIN)
  install(TARGETS remfile_vfd_plugin
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/hdf5/plugin"
  )
endif()
