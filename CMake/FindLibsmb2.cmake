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
# Find the libsmb2 library
#
# Input variables:
#
# - `LIBSMB2_INCLUDE_DIR`:      Absolute path to libsmb2 include dir.
# - `LIBSMB2_LIBRARY`:          Absolute path to `smb2` library.
# - `LIBSMB2_USE_STATIC_LIBS`:  Configure for static libsmb2.
#
# Defines:
#
# - `LIBSMB2_FOUND`:            System has libsmb2.
# - `LIBSMB2_VERSION`:          Version of libsmb2.
# - `CURL::libsmb2`:            libsmb2 library target.

set(_libsmb2_pc_requires "libsmb2")

if(NOT DEFINED LIBSMB2_INCLUDE_DIR AND
   NOT DEFINED LIBSMB2_LIBRARY)
  if(CURL_USE_PKGCONFIG)
    find_package(PkgConfig QUIET)
    pkg_check_modules(_libsmb2 ${_libsmb2_pc_requires})
  endif()
  if(NOT _libsmb2_FOUND AND CURL_USE_CMAKECONFIG)
    find_package(libsmb2 CONFIG QUIET)
  endif()
endif()

if(_libsmb2_FOUND AND _libsmb2_INCLUDE_DIRS)
  set(Libsmb2_FOUND TRUE)
  set(LIBSMB2_FOUND TRUE)
  set(LIBSMB2_VERSION ${_libsmb2_VERSION})
  if(LIBSMB2_USE_STATIC_LIBS)
    set(_libsmb2_CFLAGS       "${_libsmb2_STATIC_CFLAGS}")
    set(_libsmb2_INCLUDE_DIRS "${_libsmb2_STATIC_INCLUDE_DIRS}")
    set(_libsmb2_LIBRARY_DIRS "${_libsmb2_STATIC_LIBRARY_DIRS}")
    set(_libsmb2_LIBRARIES    "${_libsmb2_STATIC_LIBRARIES}")
  endif()
  message(STATUS "Found Libsmb2 (via pkg-config): ${_libsmb2_INCLUDE_DIRS} (found version \"${LIBSMB2_VERSION}\")")
else()
  find_path(LIBSMB2_INCLUDE_DIR NAMES "smb2/libsmb2.h")
  if(LIBSMB2_USE_STATIC_LIBS)
    find_library(LIBSMB2_LIBRARY NAMES "smb2_static" "smb2")
  else()
    find_library(LIBSMB2_LIBRARY NAMES "smb2")
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Libsmb2
    REQUIRED_VARS
      LIBSMB2_INCLUDE_DIR
      LIBSMB2_LIBRARY
  )

  if(LIBSMB2_FOUND)
    set(_libsmb2_INCLUDE_DIRS ${LIBSMB2_INCLUDE_DIR})
    set(_libsmb2_LIBRARIES    ${LIBSMB2_LIBRARY})
  endif()

  mark_as_advanced(LIBSMB2_INCLUDE_DIR LIBSMB2_LIBRARY)
endif()

if(LIBSMB2_FOUND)
  if(NOT TARGET CURL::libsmb2)
    add_library(CURL::libsmb2 INTERFACE IMPORTED)
    set_target_properties(CURL::libsmb2 PROPERTIES
      INTERFACE_LIBCURL_PC_MODULES "${_libsmb2_pc_requires}"
      INTERFACE_COMPILE_OPTIONS "${_libsmb2_CFLAGS}"
      INTERFACE_INCLUDE_DIRECTORIES "${_libsmb2_INCLUDE_DIRS}"
      INTERFACE_LINK_DIRECTORIES "${_libsmb2_LIBRARY_DIRS}"
      INTERFACE_LINK_LIBRARIES "${_libsmb2_LIBRARIES}")
  endif()
endif()
