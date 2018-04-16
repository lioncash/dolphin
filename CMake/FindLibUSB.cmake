# - Find libusb-1.0 library
# This module defines
#  LIBUSB_INCLUDE_DIR, where to find bluetooth.h
#  LIBUSB_LIBRARIES, the libraries needed to use libusb-1.0.
#  LIBUSB_FOUND, If false, do not try to use libusb-1.0.
#
# Copyright (c) 2009, Michal Cihar, <michal@cihar.com>

if(ANDROID)
  set(LIBUSB_FOUND FALSE CACHE INTERNAL "libusb-1.0 found")
  message(STATUS "libusb-1.0 not found.")
elseif (NOT LIBUSB_FOUND)
  pkg_check_modules (LIBUSB_PKG libusb-1.0)

  find_path(LIBUSB_INCLUDE_DIR
    NAMES
      libusb.h
    PATHS
      ${LIBUSB_PKG_INCLUDE_DIRS}
      /usr/include/libusb-1.0
      /usr/include
      /usr/local/include/libusb-1.0
      /usr/local/include
  )

  find_library(LIBUSB_LIBRARIES
    NAMES
      usb-1.0
      usb
    PATHS
      ${LIBUSB_PKG_LIBRARY_DIRS}
      /usr/lib
      /usr/local/lib
  )

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(LibUSB DEFAULT_MSG
    LIBUSB_LIBRARIES LIBUSB_INCLUDE_DIR
  )

  mark_as_advanced(LIBUSB_INCLUDE_DIR LIBUSB_LIBRARIES)
endif()

