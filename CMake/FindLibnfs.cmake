#***************************************************************************
#                                  _   _ ____  _
#  Project                     ___| | | |  _ \| |
#                             / __| | | | |_) | |
#                            | (__| |_| |  _ <| |___
#                             \___|\___/|_| \_\_____|
#
# Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution. The terms
# are also available at https://curl.se/docs/copyright.html.
#
# You may opt to use, copy, modify, merge, publish, distribute and/or sell
# copies of the Software, and permit persons to whom the Software is
# furnished to do so, under the terms of the COPYING file.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
# SPDX-License-Identifier: curl
#
###########################################################################
# Find the libnfs library
#
# Input variables:
#
# - `LIBNFS_INCLUDE_DIR`:      Absolute path to libnfs include directory.
# - `LIBNFS_LIBRARY`:          Absolute path to `nfs` library.
# - `LIBNFS_USE_STATIC_LIBS`:  Configure for static libnfs libraries.
#
# Defines:
#
# - `LIBNFS_FOUND`:            System has libnfs.
# - `LIBNFS_VERSION`:          Version of libnfs.
# - `CURL::libnfs`:            libnfs library target.

set(_libnfs_pc_requires "libnfs")

if(NOT DEFINED LIBNFS_INCLUDE_DIR AND
   NOT DEFINED LIBNFS_LIBRARY)
  if(CURL_USE_PKGCONFIG)
    find_package(PkgConfig QUIET)
    pkg_check_modules(_libnfs ${_libnfs_pc_requires})
  endif()
  if(NOT _libnfs_FOUND AND CURL_USE_CMAKECONFIG)
    find_package(libnfs CONFIG QUIET)
  endif()
endif()

if(_libnfs_FOUND AND _libnfs_INCLUDE_DIRS)
  set(Libnfs_FOUND TRUE)
  set(LIBNFS_FOUND TRUE)
  set(LIBNFS_VERSION ${_libnfs_VERSION})
  if(LIBNFS_USE_STATIC_LIBS)
    set(_libnfs_CFLAGS       "${_libnfs_STATIC_CFLAGS}")
    set(_libnfs_INCLUDE_DIRS "${_libnfs_STATIC_INCLUDE_DIRS}")
    set(_libnfs_LIBRARY_DIRS "${_libnfs_STATIC_LIBRARY_DIRS}")
    set(_libnfs_LIBRARIES    "${_libnfs_STATIC_LIBRARIES}")
  endif()
  message(STATUS "Found Libnfs (via pkg-config): ${_libnfs_INCLUDE_DIRS} (found version \"${LIBNFS_VERSION}\")")
elseif(libnfs_CONFIG)
  set(Libnfs_FOUND TRUE)
  set(LIBNFS_FOUND TRUE)
  set(LIBNFS_VERSION ${libnfs_VERSION})
  set(_libnfs_LIBRARIES libnfs::nfs)
  message(STATUS "Found Libnfs (via CMake Config): ${libnfs_CONFIG} (found version \"${LIBNFS_VERSION}\")")
else()
  find_path(LIBNFS_INCLUDE_DIR NAMES "nfsc/libnfs.h")
  if(LIBNFS_USE_STATIC_LIBS)
    find_library(LIBNFS_LIBRARY NAMES "nfs_static" "libnfs_static" "nfs" "libnfs")
  else()
    find_library(LIBNFS_LIBRARY NAMES "nfs" "libnfs")
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Libnfs
    REQUIRED_VARS
      LIBNFS_INCLUDE_DIR
      LIBNFS_LIBRARY
  )

  if(LIBNFS_FOUND)
    set(_libnfs_INCLUDE_DIRS ${LIBNFS_INCLUDE_DIR})
    set(_libnfs_LIBRARIES    ${LIBNFS_LIBRARY})
  endif()

  mark_as_advanced(LIBNFS_INCLUDE_DIR LIBNFS_LIBRARY)
endif()

if(LIBNFS_FOUND)
  if(NOT TARGET CURL::libnfs)
    add_library(CURL::libnfs INTERFACE IMPORTED)
    set_target_properties(CURL::libnfs PROPERTIES
      INTERFACE_LIBCURL_PC_MODULES "${_libnfs_pc_requires}"
      INTERFACE_COMPILE_OPTIONS "${_libnfs_CFLAGS}"
      INTERFACE_INCLUDE_DIRECTORIES "${_libnfs_INCLUDE_DIRS}"
      INTERFACE_LINK_DIRECTORIES "${_libnfs_LIBRARY_DIRS}"
      INTERFACE_LINK_LIBRARIES "${_libnfs_LIBRARIES}")
  endif()
endif()
