cmake_minimum_required(VERSION 3.18)
project(mooncake_nds_store_adapter LANGUAGES CXX)

set(NDS_SOURCE "" CACHE PATH "NDS source tree containing include/aclfile.h")
set(NDS_BUILD "" CACHE PATH "NDS shared library build directory")
set(CANN_ROOT "" CACHE PATH "CANN runtime root")

find_path(ACLFILE_INCLUDE_DIR aclfile.h HINTS "${NDS_SOURCE}/include" REQUIRED)
find_library(ACLFILE_LIBRARY aclfile HINTS "${NDS_BUILD}" REQUIRED)
find_library(ACLFILE_ASCEND_LIBRARY aclfile_ascend HINTS "${NDS_BUILD}" REQUIRED)
find_path(CANN_INCLUDE_DIR acl/acl.h HINTS "${CANN_ROOT}/include" REQUIRED)
find_library(ASCENDCL_LIBRARY ascendcl HINTS "${CANN_ROOT}/lib64" REQUIRED)
find_package(Threads REQUIRED)

add_library(nds_mooncake_primary SHARED
  nds_driver.cpp
  store_nds_restore.cpp
  store_nds_write.cpp)
target_compile_features(nds_mooncake_primary PRIVATE cxx_std_17)
target_include_directories(nds_mooncake_primary PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${ACLFILE_INCLUDE_DIR}"
  "${CANN_INCLUDE_DIR}")
target_link_libraries(nds_mooncake_primary PRIVATE
  "${ACLFILE_ASCEND_LIBRARY}"
  "${ACLFILE_LIBRARY}"
  "${ASCENDCL_LIBRARY}"
  Threads::Threads)
target_link_options(nds_mooncake_primary PRIVATE -Wl,-z,defs)
