%bcond_with wayland

Name: data-provider-master
Summary: Master service provider for liveboxes
Version: 0.42.1
Release: 1
Group: HomeTF/Livebox
License: Flora
Source0: %{name}-%{version}.tar.gz
Source1001: %{name}.manifest
BuildRequires: cmake, gettext-tools, smack, coreutils
BuildRequires: pkgconfig(ail)
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
BuildRequires: pkgconfig(livebox-service)
BuildRequires: pkgconfig(notification)
BuildRequires: pkgconfig(notification-service)
BuildRequires: pkgconfig(badge)
BuildRequires: pkgconfig(badge-service)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(security-server)
Requires(post): sys-assert
Requires(post): dbus

%description
Manage the 2nd stage livebox service provider and communicate with the viewer application.
Keep trace on the life-cycle of the livebox and status of the service providers, viewer applications.

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
export LIVEBOX_SHM=private
%endif

%cmake . -DPRODUCT=${LIVEBOX_SHM} -DENGINEER_BINARY=${ENGINEER} -DWAYLAND_SUPPORT=${WAYLAND_SUPPORT} -DX11_SUPPORT=${X11_SUPPORT}

CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license
mkdir -p %{buildroot}/opt/usr/share/live_magazine
mkdir -p %{buildroot}/opt/usr/share/live_magazine/log
mkdir -p %{buildroot}/opt/usr/share/live_magazine/reader
mkdir -p %{buildroot}/opt/usr/share/live_magazine/always
mkdir -p %{buildroot}/opt/usr/devel/usr/bin
mkdir -p %{buildroot}/opt/dbspace
mkdir -p %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants
touch %{buildroot}/opt/dbspace/.livebox.db
touch %{buildroot}/opt/dbspace/.livebox.db-journal
ln -sf ../data-provider-master.service %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants/data-provider-master.service
if [ ! -s %{buildroot}/opt/dbspace/.livebox.db ]; then
echo "LiveBox DB file is not exists, initiate it"
sqlite3 %{buildroot}/opt/dbspace/.livebox.db <<EOF
CREATE TABLE version ( version INTEGER );
CREATE TABLE box_size ( pkgid TEXT NOT NULL, size_type INTEGER, preview TEXT, touch_effect INTEGER, need_frame INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE client (pkgid TEXT PRIMARY KEY NOT NULL, icon TEXT, name TEXT, auto_launch TEXT, pd_size TEXT, content TEXT, nodisplay INTEGER, setup TEXT, mouse_event INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupinfo ( id INTEGER PRIMARY KEY AUTOINCREMENT, cluster TEXT NOT NULL, category TEXT NOT NULL, pkgid TEXT NOT NULL, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE groupmap (option_id INTEGER PRIMARY KEY AUTOINCREMENT, id INTEGER, pkgid TEXT NOT NULL, ctx_item TEXT NOT NULL, FOREIGN KEY(id) REFERENCES groupinfo(id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE i18n ( pkgid TEXT NOT NULL, lang TEXT COLLATE NOCASE, name TEXT, icon TEXT, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE option ( pkgid TEXT NOT NULL, option_id INTEGER, key TEXT NOT NULL, value TEXT NOT NULL, FOREIGN KEY(option_id) REFERENCES groupmap(option_id), FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
CREATE TABLE pkgmap ( pkgid TEXT PRIMARY KEY NOT NULL, appid TEXT, uiapp TEXT, prime INTEGER, category TEXT DEFAULT 'http://tizen.org/category/default' );
CREATE TABLE provider ( pkgid TEXT PRIMARY KEY NOT NULL, network INTEGER, abi TEXT, secured INTEGER, box_type INTEGER, box_src TEXT, box_group TEXT, pd_type INTEGER, pd_src TEXT, pd_group TEXT, libexec TEXT, timeout INTEGER, period TEXT, script TEXT, pinup INTEGER, FOREIGN KEY(pkgid) REFERENCES pkgmap(pkgid) ON DELETE CASCADE);
EOF
fi

%pre
# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/data-provider-master ]; then
	%{_sysconfdir}/rc.d/init.d/data-provider-master stop
fi

%post
chown 5000:5000 /opt/usr/share/live_magazine
chmod 750 /opt/usr/share/live_magazine
chown 5000:5000 /opt/usr/share/live_magazine/log
chmod 750 /opt/usr/share/live_magazine/log
chown 5000:5000 /opt/usr/share/live_magazine/reader
chmod 750 /opt/usr/share/live_magazine/reader
chown 5000:5000 /opt/usr/share/live_magazine/always
chmod 750 /opt/usr/share/live_magazine/always
chown 0:5000 /opt/dbspace/.livebox.db
chmod 640 /opt/dbspace/.livebox.db
chown 0:5000 /opt/dbspace/.livebox.db-journal
chmod 640 /opt/dbspace/.livebox.db-journal
vconftool set -t bool "memory/data-provider-master/started" 0 -i -u 5000 -f -s system::vconf_system
vconftool set -t int "memory/private/data-provider-master/restart_count" 0 -i -u 5000 -f -s data-provider-master
vconftool set -t string "db/data-provider-master/serveraddr" "/opt/usr/share/live_magazine/.client.socket" -i -u 5000 -f -s system::vconf_system
echo "Successfully installed. Please start a daemon again manually"
echo "%{_sysconfdir}/init.d/data-provider-master start"

%files -n data-provider-master
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_bindir}/data-provider-master
%{_prefix}/etc/package-manager/parserlib/*
%{_datarootdir}/data-provider-master/*
%{_libdir}/systemd/system/multi-user.target.wants/data-provider-master.service
%{_libdir}/systemd/system/data-provider-master.service
%{_datarootdir}/license/*
/opt/usr/share/live_magazine/*
/opt/etc/dump.d/module.d/dump_livebox.sh
%if 0%{?tizen_build_binary_release_type_eng}
/opt/usr/devel/usr/bin/*
%endif
/opt/dbspace/.livebox.db
/opt/dbspace/.livebox.db-journal

# End of a file
