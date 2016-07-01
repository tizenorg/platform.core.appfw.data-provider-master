%bcond_with wayland

Name: data-provider-master
Summary: Master service provider for badge, shortcut, notification
Version: 1.3.0
Release: 1
Group: Applications/Core Applications
License: Apache-2.0
Source0: %{name}-%{version}.tar.gz
Source1: data-provider-master.service
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
BuildRequires: pkgconfig(libxml-2.0)
BuildRequires: pkgconfig(pkgmgr)
BuildRequires: pkgconfig(pkgmgr-info)
BuildRequires: pkgconfig(notification)
BuildRequires: pkgconfig(badge)
%if "%{profile}" != "wearable"
BuildRequires: pkgconfig(shortcut)
%endif
BuildRequires: pkgconfig(libsystemd-daemon)
BuildRequires: pkgconfig(cynara-client)
BuildRequires: pkgconfig(cynara-session)
BuildRequires: pkgconfig(cynara-creds-socket)
BuildRequires: pkgconfig(alarm-service)

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

%if "%{profile}" == "wearable"
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
mkdir -p %{buildroot}%{_prefix}/lib/systemd/system/multi-user.target.wants
install -m 0644 %SOURCE1 %{buildroot}%{_unitdir}/data-provider-master.service
ln -sf ../%{name}.service %{buildroot}%{_prefix}/lib/systemd/system/multi-user.target.wants/%{name}.service

%post
%files -n %{name}
%manifest %{name}.manifest
%defattr(-,root,root,-)

%attr(0755,root,root) %{_bindir}/data-provider-master
%attr(0644,root,root) %{_unitdir}/data-provider-master.service
%{_unitdir}/multi-user.target.wants/data-provider-master.service
%attr(0644,root,root) %{_datadir}/dbus-1/system-services/org.tizen.data-provider-master.service
%config %{_sysconfdir}/dbus-1/system.d/data-provider-master.conf
%{_prefix}/bin/%{name}
%{_datarootdir}/license/*
%if 0%{?tizen_build_binary_release_type_eng}
%endif
#%defattr(-,owner,users,-)

# End of a file
