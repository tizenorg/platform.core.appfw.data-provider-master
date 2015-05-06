#!/bin/sh

WIDGET_DEBUG=$1/widget
mkdir -p ${WIDGET_DEBUG}
/bin/cp -r /opt/usr/share/live_magazine ${WIDGET_DEBUG}
/bin/cp -r /tmp/.widget.service ${WIDGET_DEBUG}/log
/bin/cp -r /opt/dbspace/.widget.db ${WIDGET_DEBUG}
ls -la /tmp/ | /bin/grep srw > ${WIDGET_DEBUG}/log/tmp.hidden_files
