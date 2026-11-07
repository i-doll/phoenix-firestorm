#!/bin/bash

# Send a URL of the form secondlife://... to the viewer.
#

URL="$1"

if [ -z "$URL" ]; then
    #echo Usage: $0 secondlife://...
    echo "Usage: $0 [ secondlife://  | hop:// ] ..."
    exit
fi

RUN_PATH=`dirname "$0" || echo .`
#cd "${RUN_PATH}/.."
cd "${RUN_PATH}"

#exec ./idyll -url \'"${URL}"\'
if [ `pidof do-not-directly-run-idyll-bin` ]; then
	exec dbus-send --type=method_call --dest=org.idyllviewer.ViewerAppAPIService /org/idyllviewer/ViewerAppAPI org.idyllviewer.ViewerAppAPI.GoSLURL string:"$1"
else
	exec ../idyll -url \'"${URL}"\'
fi
