%bcond_with wayland

Name: data-provider-master
Summary: Master service provider for widgetes
Version: 1.1.6
Release: 1
Group: HomeTF/widget
License: Flora License, Version 1.1
Source0: %{name}-%{version}.tar.gz
Source1001: %{name}.manifest
BuildRequires: cmake, gettext-tools, smack, coreutils
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(aul)
BuildRequires: pkgconfig(vconf)
BuildRequires: pkgconfig(sqlite3)
BuildRequires: pkgconfig(db-util)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(libsmack)
BuildRequires: pkgconfig(bundle)

%if %{with wayland}
BuildRequires: pkgconfig(ecore-wayland)
%else
BuildRequires: pkgconfig(ecore-x)
BuildRequires: pkgconfig(x11)
BuildRequires: pkgconfig(libdri2)
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libtbm)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(dri2proto)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xdamage)
%endif

BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(eina)
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(libxml-2.0)
BuildRequires: pkgconfig(pkgmgr)
BuildRequires: pkgconfig(pkgmgr-info)
BuildRequires: pkgconfig(widget_service)
BuildRequires: pkgconfig(notification)
BuildRequires: pkgconfig(notification-service)
BuildRequires: pkgconfig(badge)
BuildRequires: pkgconfig(badge-service)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(security-server)
BuildRequires: pkgconfig(libsystemd-daemon)
Requires(post): sys-assert
Requires(post): dbus

%description
Manage the 2nd stage widget service provider and communicate with the viewer application.
Keep trace on the life-cycle of the widget and status of the service providers, viewer applications.

%prep
%setup -q
cp %{SOURCE1001} .

%build
%if 0%{?sec_build_binary_debug_enable}
export CFLAGS="$CFLAGS -DTIZEN_DEBUG_ENABLE"
export CXXFLAGS="$CXXFLAGS -DTIZEN_DEBUG_ENABLE"
export FFLAGS="$FFLAGS -DTIZEN_DEBUG_ENABLE"
%endif

export ENGINEER=false
%if 0%{?tizen_build_binary_release_type_eng}
export CFLAGS="${CFLAGS} -DTIZEN_ENGINEER_MODE"
export CXXFLAGS="${CXXFLAGS} -DTIZEN_ENGINEER_MODE"
export FFLAGS="${FFLAGS} -DTIZEN_ENGINEER_MODE"
export ENGINEER=true
%endif

%if %{with wayland}
export WAYLAND_SUPPORT=On
export X11_SUPPORT=Off
export LIVEBOX_SHM=wayland
%else
export WAYLAND_SUPPORT=Off
export X11_SUPPORT=On
export LIVEBOX_SHM=x11
%endif

%if "%{_repository}" == "wearable"
export LIVEBOX_SHM="${LIVEBOX_SHM}.wearable"
export MOBILE=Off
export WEARABLE=On
%else
export LIVEBOX_SHM="${LIVEBOX_SHM}.mobile"
export MOBILE=On
export WEARABLE=Off
%endif

export LIVEBOX_SHM="${LIVEBOX_SHM}.480x800"
export LIVEBOX=On

%ifarch %ix86
export TARGET=emulator
%else
export TARGET=device
%endif

%cmake . -DNAME=%{name} -DPRODUCT=${LIVEBOX_SHM} -DENGINEER_BINARY=${ENGINEER} -DWAYLAND_SUPPORT=${WAYLAND_SUPPORT} -DX11_SUPPORT=${X11_SUPPORT} -DMOBILE=${MOBILE} -DWEARABLE=${WEARABLE} -DLIVEBOX=${LIVEBOX} -DTARGET=${TARGET}

CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants
ln -sf ../%{name}.service %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants/%{name}.service
mkdir -p %{buildroot}/opt/usr/share/live_magazine
mkdir -p %{buildroot}/opt/usr/share/live_magazine/log
mkdir -p %{buildroot}/opt/usr/share/live_magazine/reader
mkdir -p %{buildroot}/opt/usr/share/live_magazine/always
mkdir -p %{buildroot}/opt/usr/devel/usr/bin
mkdir -p %{buildroot}/opt/dbspace

echo "widget DB file is not exists, initiate it"
sqlite3 %{buildroot}/opt/dbspace/.widget.db-new <<EOF
CREATE TABLE version ( version INTEGER );
CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, preview TEXT, touch_effect INTEGER, need_frame INTEGER, mouse_event INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE client (pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch TEXT, gbar_size TEXT, content TEXT, nodisplay INTEGER, setup TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, pkgid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupmap (option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT COLLATE NOCASE, name TEXT, icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, FOREIGN KEY(option_id) REFERENCES groupmap(option_id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, uiapp TEXT, prime INTEGER, category TEXT DEFAULT 'http://tizen.org/category/default' );
CREATE TABLE provider ( pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, gbar_type INTEGER, gbar_src TEXT, gbar_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, count INTEGER, direct_input INTEGER DEFAULT 0, hw_acceleration TEXT DEFAULT '', FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
EOF

%pre
# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/%{name} ]; then
	%{_sysconfdir}/rc.d/init.d/%{name} stop
fi

%post
#
# NOTE:
# This SYSTEM_UID should be updated properly.
# In the SPIN, system user id is 1000
SYSTEM_UID=1000
APP_UID=5000
APP_GID=5000

if [ ! -s /opt/dbspace/.widget.db ]; then
	echo "DB is not exists"
	mv /opt/dbspace/.widget.db-new /opt/dbspace/.widget.db
	mv /opt/dbspace/.widget.db-new-journal /opt/dbspace/.widget.db-journal
else
	VERSION=`sqlite3 /opt/dbspace/.widget.db "SELECT * FROM version"`
	echo "DB is already exists (Version: $VERSION)"
	echo "==============================================="
	sqlite3 /opt/dbspace/.widget.db "SELECT * FROM pkgmap"
	echo "==============================================="
	rm -rf /opt/dbspace/.widget.db-new
	rm -rf /opt/dbspace/.widget.db-new-journal
fi

chown ${APP_UID}:${APP_GID} /opt/usr/share/live_magazine
# System tool(widget-mgr) should be able to access this folder.
# So give the "rx" permission to the other group. (750 -> 755)
chmod 755 /opt/usr/share/live_magazine
chown ${APP_UID}:${APP_GID} /opt/usr/share/live_magazine/log
chmod 750 /opt/usr/share/live_magazine/log
chown ${APP_UID}:${APP_GID} /opt/usr/share/live_magazine/reader
chmod 750 /opt/usr/share/live_magazine/reader
chown ${APP_UID}:${APP_GID} /opt/usr/share/live_magazine/always
chmod 750 /opt/usr/share/live_magazine/always
chown ${SYSTEM}:${APP_GID} /opt/dbspace/.widget.db
chmod 640 /opt/dbspace/.widget.db
chown ${SYSTEM}:${APP_GID} /opt/dbspace/.widget.db-journal
chmod 640 /opt/dbspace/.widget.db-journal

vconftool set -t bool "memory/%{name}/started" 0 -i -u ${APP_UID} -f -s system::vconf_system
vconftool set -t int "memory/private/%{name}/restart_count" 0 -i -u ${APP_UID} -f -s %{name}
vconftool set -t string "db/%{name}/serveraddr" "/opt/usr/share/live_magazine/.client.socket" -i -u ${APP_UID} -f -s system::vconf_system

echo "Successfully installed. Please start a daemon again manually"

%files -n %{name}
%manifest %{name}.manifest
%defattr(-,system,system,-)
%caps(cap_chown,cap_dac_override,cap_dac_read_search,cap_sys_admin,cap_sys_nice,cap_mac_override,cap_mac_admin+ep) %{_bindir}/%{name}
%{_libdir}/systemd/system/multi-user.target.wants/%{name}.service
%{_libdir}/systemd/system/%{name}.service
%{_datarootdir}/license/*
%if 0%{?tizen_build_binary_release_type_eng}
/opt/usr/devel/usr/bin/*
%endif
%{_prefix}/etc/package-manager/parserlib/*
%{_datarootdir}/%{name}/*
/opt/etc/dump.d/module.d/dump_widget.sh
/opt/usr/share/live_magazine/*
/opt/dbspace/.widget.db*
%{_sysconfdir}/smack/accesses.d/%{name}

# End of a file
