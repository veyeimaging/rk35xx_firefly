#!/bin/sh

export DISPLAY=:0.0
export XAUTHORITY=/home/firefly/.Xauthority
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/lib/aarch64-linux-gnu/gstreamer-1.0

export XDG_RUNTIME_DIR=/run/user/1000
WIDTH=1920
HEIGHT=1080
SINK_356x=rkximagesink
SINK_3588=glimagesink
#SINK=kmssink

echo "Start MIPI CSI Camera Preview!"

if cat /proc/device-tree/model | grep -q "3588" ;then
        gst-launch-1.0 v4l2src device=/dev/video0 io-mode=4 ! queue ! video/x-raw,format=NV12,width=${WIDTH},height=${HEIGHT},framerate=30/1  ! $SINK_3588&
else
        gst-launch-1.0 v4l2src device=/dev/video0 io-mode=4 ! videoconvert ! video/x-raw,format=NV12,width=${WIDTH},height=${HEIGHT}  ! $SINK_356x&
fi
wait
