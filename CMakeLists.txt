CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(data-provider-master C)

INCLUDE(FindPkgConfig)
pkg_check_modules(pkg REQUIRED
	ail
	dlog
	aul
	vconf
	sqlite3
	db-util
	glib-2.0
	gio-2.0
	bundle
	ecore-x
	ecore
	evas
	ecore-evas
	capi-context-engine
	com-core
)

SET(PACKAGE "${PROJECT_NAME}")
SET(LOCALEDIR "/opt/apps/com.samsung.${PROJECT_NAME}/res/locale")
SET(CMAKE_C_FLAGS "-Wall -fpie -Winline -Werror -fno-builtin-malloc -fno-omit-frame-pointer")
SET(CMAKE_EXE_LINKER_FLAGS "-pie")

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/include)

ADD_DEFINITIONS("-DPREFIX=\"${CMAKE_INSTALL_PREFIX}\"")
ADD_DEFINITIONS("-DPATH_MAX=256")
ADD_DEFINITIONS("-DPACKAGE=\"${PACKAGE}\"")
ADD_DEFINITIONS("-DLOCALEDIR=\"${LOCALEDIR}\"")

ADD_DEFINITIONS("-DSLAVE_PKGNAME=\"com.samsung.data-provider-slave\"")

ADD_DEFINITIONS("-DNDEBUG")
#ADD_DEFINITIONS("-DFLOG")
ADD_DEFINITIONS("-DLOG_TAG=\"${PROJECT_NAME}\"")
ADD_DEFINITIONS(${pkg_CFLAGS})
ADD_DEFINITIONS(${pkg_LDFLAGS})


ADD_EXECUTABLE(${PROJECT_NAME}
	src/main.c
	src/util.c
	src/fault_manager.c
	src/parser.c
	src/conf.c
	src/dead_monitor.c
	src/ctx_client.c
	src/group.c
	src/fb.c
	src/script_handler.c
	src/buffer_handler.c
	src/io.c
	src/virtual_canvas.c
	src/xmonitor.c
	src/slave_life.c
	src/slave_rpc.c
	src/client_life.c
	src/client_rpc.c
	src/setting.c
	src/package.c
	src/instance.c
	src/server.c
)

TARGET_LINK_LIBRARIES(${PROJECT_NAME} ${pkg_LDFLAGS} "-ldl")
#INSTALL(FILES ${CMAKE_SOURCE_DIR}/com.samsung.data-provider-master.desktop DESTINATION /opt/share/applications)
#INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /opt/apps/com.samsung.${PROJECT_NAME}/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
INSTALL(TARGETS ${PROJECT_NAME} DESTINATION /usr/bin PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# INCLUDE FOR BUILD & INSTALL .PO FILES
ADD_SUBDIRECTORY(res)
ADD_SUBDIRECTORY(data)
