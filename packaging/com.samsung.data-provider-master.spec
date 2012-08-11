Name: com.samsung.data-provider-master
Summary: Master data provider
Version: 0.7.0
Release: 1
Group: main/app
License: Samsung Proprietary License
Source0: %{name}-%{version}.tar.gz
BuildRequires: cmake, gettext-tools
BuildRequires: pkgconfig(ail)
BuildRequires: pkgconfig(dlog)
BuildRequires: pkgconfig(aul)
BuildRequires: pkgconfig(vconf)
BuildRequires: pkgconfig(sqlite3)
BuildRequires: pkgconfig(db-util)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(bundle)
BuildRequires: pkgconfig(ecore-x)
BuildRequires: pkgconfig(ecore)
BuildRequires: pkgconfig(evas)
BuildRequires: pkgconfig(ecore-evas)
BuildRequires: pkgconfig(capi-context-engine)
BuildRequires: pkgconfig(com-core)
BuildRequires: pkgconfig(heynoti)
BuildRequires: pkgconfig(libxml-2.0)

%description
Manage the slave data provider and communicate with client applications.

%prep
%setup -q

%build
cmake . -DCMAKE_INSTALL_PREFIX=%{_prefix}
CFLAGS="${CFLAGS} -Wall -Winline -Werror" LDFLAGS="${LDFLAGS}" make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%post
mkdir -p /opt/share/live_magazine/log
mkdir -p /opt/share/live_magazine/reader
chown 5000:5000 /opt/share/live_magazine
chown 5000:5000 /opt/share/live_magazine/log
chown 5000:5000 /opt/share/live_magazine/reader

# Menu daemon will launch the data provider master automatically
#if [ -f "/etc/rc.d/rc3.d/S41data-provider-master" ]; then
#	rm -f /etc/rc.d/rc3.d/S41data-provider-master
#fi

ln -sf /etc/rc.d/init.d/data-provider-master /etc/rc.d/rc3.d/S99data-provider-master

TMP=`which ps`
if [ $? -ne 0 ]; then
	echo "'ps' is not exists"
	exit 0
fi

TMP=`which aul_test`
if [ $? -ne 0 ]; then
	echo "'aul_test' is not exists"
	exit 0
fi

PID=`ps ax | grep 'data-provider-master' | grep -v 'grep' | grep -v 'rpm' | grep -v 'dlogutil' | awk '{print $1}'`
if [ x"$PID" != x"" ]; then
	aul_test term_pid $PID
	sleep 1
fi

PID_LIST=`ps ax | grep 'data-provider-slave' | grep -v 'grep' | grep -v 'rpm' | grep -v 'dlogutil' | awk '{print $1}'`
for PID in $PID_LIST
do
	echo "Terminate old data provider slave $PID"
	kill -9 $PID
done

#
# menu-daemon will relaunch the data provider master
#
# aul_test launch com.samsung.data-provider-master

%files -n com.samsung.data-provider-master
#%manifest com.samsung.data-provider-master.manifest
%defattr(-,root,root,-)
#/opt/apps/com.samsung.data-provider-master/bin/data-provider-master
#/opt/share/applications/com.samsung.data-provider-master.desktop
/etc/rc.d/init.d/data-provider-master
/usr/bin/data-provider-master
/usr/etc/package-manager/parserlib/*
/usr/share/data-provider-master/*
