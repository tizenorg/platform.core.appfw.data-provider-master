CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(${NAME} C)

SET(PREFIX ${CMAKE_INSTALL_PREFIX})
SET(EXEC_PREFIX "\${prefix}")
SET(LIBDIR ${LIB_INSTALL_DIR})
SET(INCLUDEDIR "\${prefix}/include")


INCLUDE(FindPkgConfig)
pkg_check_modules(pkg REQUIRED
	dlog
	aul
	vconf
	sqlite3
	db-util
	glib-2.0
	gio-2.0
	bundle
	ecore
	eina
	pkgmgr
	notification
	badge
	libsmack
	shortcut
	pkgmgr-info
	libsystemd-daemon
	capi-appfw-app-manager
	cynara-client
	cynara-session
	cynara-creds-socket
)

SET(PACKAGE "${PROJECT_NAME}")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Werror -fno-builtin-malloc -fno-omit-frame-pointer -g -fPIE")

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)

#ADD_DEFINITIONS("-DPREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
ADD_DEFINITIONS("-DPATH_MAX=256")
ADD_DEFINITIONS("-DPACKAGE=\"${PACKAGE}\"")
ADD_DEFINITIONS("-DLOCALEDIR=\"${LOCALEDIR}\"")

ADD_DEFINITIONS("-D_GNU_SOURCE")
ADD_DEFINITIONS("-D_FILE_OFFSET_BITS=64")

ADD_DEFINITIONS("-DSHORTCUT_SMACK_LABEL=NULL")
ADD_DEFINITIONS("-DNOTIFICATION_SMACK_LABEL=NULL")
ADD_DEFINITIONS("-DBADGE_SMACK_LABEL=NULL")
ADD_DEFINITIONS("-DDATA_SHARE_LABEL=NULL")
ADD_DEFINITIONS("-DDEFAULT_SMACK_LABEL=NULL")

ADD_DEFINITIONS("-DNDEBUG")

ADD_DEFINITIONS("-D_USE_ECORE_TIME_GET")

IF (MOBILE)
	ADD_DEFINITIONS("-DMOBILE")
ELSEIF (WEARABLE)
	ADD_DEFINITIONS("-DWEARABLE")
ENDIF (MOBILE)

SET(BUILD_SOURCE
	src/main.c
	src/util.c
	src/pkgmgr.c
	src/shortcut_service.c
	src/badge_service.c
	src/notification_service.c
	src/service_common.c
	src/critical_log.c
)

STRING(REPLACE "-L-l" "-l" pkg_fixed_LDFLAGS ${pkg_LDFLAGS})
STRING(REPLACE "-l" " -l" pkg_LDFLAGS ${pkg_fixed_LDFLAGS})
STRING(REPLACE "-L" " -L" pkg_fixed_LDFLAGS ${pkg_LDFLAGS})
STRING(REGEX REPLACE "^ " "" pkg_LDFLAGS ${pkg_fixed_LDFLAGS})

ADD_DEFINITIONS(${pkg_CFLAGS})
ADD_DEFINITIONS(${pkg_LDFLAGS})

ADD_EXECUTABLE(${PROJECT_NAME} ${BUILD_SOURCE})

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${pkg_LDFLAGS} "-ldl -lrt -pie")

INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME "${PROJECT_NAME}")
INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /usr/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

CONFIGURE_FILE(data-provider-master.conf.in data-provider-master.conf @ONLY)
CONFIGURE_FILE(org.tizen.data-provider-master.service.in org.tizen.data-provider-master.service @ONLY)

INSTALL(FILES ${CMAKE_CURRENT_SOURCE_DIR}/org.tizen.data-provider-master.service DESTINATION ${SHARE_INSTALL_PREFIX}/dbus-1/system-services/)
INSTALL(FILES ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.conf DESTINATION ${SYSCONF_INSTALL_DIR}/dbus-1/system.d/)
# End of a file
