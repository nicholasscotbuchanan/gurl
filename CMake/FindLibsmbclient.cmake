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
# Find the Samba libsmbclient library
#
# Input variables:
#
# - `LIBSMBCLIENT_INCLUDE_DIR`:      Absolute path to libsmbclient include dir.
# - `LIBSMBCLIENT_LIBRARY`:          Absolute path to `smbclient` library.
# - `LIBSMBCLIENT_USE_STATIC_LIBS`:  Configure for static libsmbclient.
#
# Defines:
#
# - `LIBSMBCLIENT_FOUND`:            System has libsmbclient.
# - `LIBSMBCLIENT_VERSION`:          Version of libsmbclient.
# - `CURL::libsmbclient`:            libsmbclient library target.

set(_libsmbclient_pc_requires "smbclient")

if(NOT DEFINED LIBSMBCLIENT_INCLUDE_DIR AND
   NOT DEFINED LIBSMBCLIENT_LIBRARY)
  if(CURL_USE_PKGCONFIG)
    find_package(PkgConfig QUIET)
    pkg_check_modules(_libsmbclient ${_libsmbclient_pc_requires})
  endif()
  if(NOT _libsmbclient_FOUND AND CURL_USE_CMAKECONFIG)
    find_package(smbclient CONFIG QUIET)
  endif()
endif()

if(_libsmbclient_FOUND AND _libsmbclient_INCLUDE_DIRS)
  set(Libsmbclient_FOUND TRUE)
  set(LIBSMBCLIENT_FOUND TRUE)
  set(LIBSMBCLIENT_VERSION ${_libsmbclient_VERSION})
  if(LIBSMBCLIENT_USE_STATIC_LIBS)
    set(_libsmbclient_CFLAGS       "${_libsmbclient_STATIC_CFLAGS}")
    set(_libsmbclient_INCLUDE_DIRS "${_libsmbclient_STATIC_INCLUDE_DIRS}")
    set(_libsmbclient_LIBRARY_DIRS "${_libsmbclient_STATIC_LIBRARY_DIRS}")
    set(_libsmbclient_LIBRARIES    "${_libsmbclient_STATIC_LIBRARIES}")
  endif()
  message(STATUS "Found Libsmbclient (via pkg-config): ${_libsmbclient_INCLUDE_DIRS} (found version \"${LIBSMBCLIENT_VERSION}\")")
else()
  find_path(LIBSMBCLIENT_INCLUDE_DIR NAMES "libsmbclient.h")
  if(LIBSMBCLIENT_USE_STATIC_LIBS)
    find_library(LIBSMBCLIENT_LIBRARY NAMES "smbclient_static" "smbclient")
  else()
    find_library(LIBSMBCLIENT_LIBRARY NAMES "smbclient")
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(Libsmbclient
    REQUIRED_VARS
      LIBSMBCLIENT_INCLUDE_DIR
      LIBSMBCLIENT_LIBRARY
  )

  if(LIBSMBCLIENT_FOUND)
    set(_libsmbclient_INCLUDE_DIRS ${LIBSMBCLIENT_INCLUDE_DIR})
    set(_libsmbclient_LIBRARIES    ${LIBSMBCLIENT_LIBRARY})
  endif()

  mark_as_advanced(LIBSMBCLIENT_INCLUDE_DIR LIBSMBCLIENT_LIBRARY)
endif()

if(LIBSMBCLIENT_FOUND)
  if(NOT TARGET CURL::libsmbclient)
    add_library(CURL::libsmbclient INTERFACE IMPORTED)
    set_target_properties(CURL::libsmbclient PROPERTIES
      INTERFACE_LIBCURL_PC_MODULES "${_libsmbclient_pc_requires}"
      INTERFACE_COMPILE_OPTIONS "${_libsmbclient_CFLAGS}"
      INTERFACE_INCLUDE_DIRECTORIES "${_libsmbclient_INCLUDE_DIRS}"
      INTERFACE_LINK_DIRECTORIES "${_libsmbclient_LIBRARY_DIRS}"
      INTERFACE_LINK_LIBRARIES "${_libsmbclient_LIBRARIES}")
  endif()
endif()
