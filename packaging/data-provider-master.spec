%bcond_with wayland

Name: data-provider-master
Summary: Master service provider for badge, shortcut, notification
Version: 1.3.0
Release: 1
Group: Applications/Core Applications
License: Flora-1.1
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
BuildRequires: pkgconfig(capi-appfw-app-manager)

BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(eina)
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(libxml-2.0)
BuildRequires: pkgconfig(pkgmgr)
BuildRequires: pkgconfig(pkgmgr-info)
BuildRequires: pkgconfig(notification)
BuildRequires: pkgconfig(badge)
BuildRequires: pkgconfig(badge-service)
BuildRequires: pkgconfig(shortcut)
BuildRequires: pkgconfig(security-server)
BuildRequires: pkgconfig(libsystemd-daemon)
BuildRequires: pkgconfig(cynara-client)
BuildRequires: pkgconfig(cynara-session)
BuildRequires: pkgconfig(cynara-creds-socket)

Requires(post): dbus

%description

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

%if "%{_repository}" == "wearable"
export MOBILE=Off
export WEARABLE=On
%else
export MOBILE=On
export WEARABLE=Off
%endif

%ifarch %ix86
export TARGET=emulator
%else
export TARGET=device
%endif

%cmake . -DNAME=%{name} -DENGINEER_BINARY=${ENGINEER} -DMOBILE=${MOBILE} -DWEARABLE=${WEARABLE} -DTARGET=${TARGET}

CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install
mkdir -p %{buildroot}/%{_datarootdir}/license
#mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/multi-user.target.wants
#ln -sf ../%{name}.service %{buildroot}%{_prefix}/lib/systemd/system/multi-user.target.wants/%{name}.service
mkdir -p %{buildroot}/opt/usr/devel/usr/bin
%pre
# Executing the stop script for stopping the service of installed provider (old version)
if [ -x %{_sysconfdir}/rc.d/init.d/%{name} ]; then
	%{_sysconfdir}/rc.d/init.d/%{name} stop
fi

%post
%files -n %{name}
%manifest %{name}.manifest
%defattr(-,root,root,-)
%{_datadir}/dbus-1/system-services/data-provider-master.service
%config %{_sysconfdir}/dbus-1/system.d/data-provider-master.conf
#%{_prefix}/lib/systemd/system/multi-user.target.wants/%{name}.service
#%{_prefix}/lib/systemd/system/%{name}.service
#%{_prefix}/lib/systemd/system/%{name}.target
%{_prefix}/bin/%{name}
%{_datarootdir}/license/*
%if 0%{?tizen_build_binary_release_type_eng}
/opt/usr/devel/usr/bin/*
%endif
#%defattr(-,owner,users,-)

# End of a file
