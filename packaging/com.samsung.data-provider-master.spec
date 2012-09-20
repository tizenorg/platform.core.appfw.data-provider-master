Name: com.samsung.data-provider-master
Summary: Master data provider
Version: 0.10.2
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
BuildRequires: pkgconfig(x11)
BuildRequires: pkgconfig(libdri2)
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libdrm_slp)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(dri2proto)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xdamage)

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
mkdir -p /opt/share/live_magazine
chown 5000:5000 /opt/share/live_magazine
chsmack -a "_" /opt/share/live_magazine
chsmack -t /opt/share/live_magazine
# According to this transmute attribute, below log, reader folder will be set as same label

mkdir /opt/share/live_magazine/log
chown 5000:5000 /opt/share/live_magazine/log

mkdir /opt/share/live_magazine/reader
chown 5000:5000 /opt/share/live_magazine/reader

# End of a list of affected folder by the transmute attribute

touch /opt/dbspace/.lviebox.db
chsmack -a "data-provider-master::db" /opt/dbspace/.livebox.db

ln -sf /etc/rc.d/init.d/data-provider-master /etc/rc.d/rc3.d/S99data-provider-master
chsmack -a "_" /etc/rc.d/rc3.d/S99data-provider-master
chsmack -e "_" /etc/rc.d/rc3.d/S99data-provider-master

TMP=`which ps`
if [ $? -ne 0 ]; then
	echo "'ps' is not exists"
	exit 0
fi

TMP=`which grep`
if [ $? -ne 0 ]; then
	echo "'grep' is not exists"
	exit 0
fi

TMP=`which awk`
if [ $? -ne 0 ]; then
	echo "'awk' is not exists"
	exit 0
fi

BIN_INODE=`stat -Lc "%i" /usr/bin/data-provider-master`

PID=`ps ax | grep 'data-provider-master' | grep -v 'grep' | grep -v 'rpm' | grep -v 'dlogutil' | awk '{print $1}'`
for I in $PID;
do
	INODE=`stat -Lc "%i"  /proc/$I/exe`
	if [ x"$BIN_INODE" == x"$INODE" ]; then
		echo "Send TERM to $I"
		kill $I # Try to terminate a master which is launched already
	fi
done

%files -n com.samsung.data-provider-master
%manifest com.samsung.data-provider-master.manifest
%defattr(-,root,root,-)
/etc/rc.d/init.d/data-provider-master
/usr/bin/data-provider-master
/usr/bin/liveinfo
/usr/etc/package-manager/parserlib/*
/usr/share/data-provider-master/*
