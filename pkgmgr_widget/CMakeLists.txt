CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(pkgmgr_plugin C)

INCLUDE(FindPkgConfig)
pkg_check_modules(bin_pkgs REQUIRED
	dlog
	sqlite3
	libxml-2.0
	db-util
	widget_service
)

FOREACH(flag ${bin_pkgs_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
ENDFOREACH(flag)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -g -Wall -Werror")
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/common/include)

ADD_SUBDIRECTORY(common)
ADD_SUBDIRECTORY(watch)
ADD_SUBDIRECTORY(widget)

ADD_DEPENDENCIES("watch-application" "common")
ADD_DEPENDENCIES("widget" "common")

# End of a file
