#!/bin/sh

LIVEBOX_DEBUG=$1/livebox
mkdir_function ${LIVEBOX_DEBUG}
/bin/cp -r /opt/usr/share/live_magazine ${LIVEBOX_DEBUG}
