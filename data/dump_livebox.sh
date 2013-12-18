#!/bin/sh

LIVEBOX_DEBUG=$1/livebox
mkdir -p ${LIVEBOX_DEBUG}
/bin/cp -r /opt/usr/share/live_magazine ${LIVEBOX_DEBUG}
/bin/cp -r /tmp/.dbox.service ${LIVEBOX_DEBUG}/log
