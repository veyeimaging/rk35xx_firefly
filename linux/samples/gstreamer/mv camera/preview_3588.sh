#!/bin/sh

export DISPLAY=:0.0
#export GST_DEBUG=*:5
#export GST_DEBUG_FILE=/tmp/2.txt

export XDG_RUNTIME_DIR=/run/user/1000

export WIDTH=1080
export HEIGHT=1080
export FPS=30

echo "Just use 1080x1080@30fps as a sample. Please modify these value."
echo "Start MIPI CSI Camera Preview!"

v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl roi_x=0

v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl roi_y=0

media-ctl -d /dev/media0 --set-v4l2 '"m00_b_mvcam 7-003b":0[fmt:UYVY8_2X8/'"$WIDTH"'x'"$HEIGHT"'@1/'"$FPS"']'

gst-launch-1.0 v4l2src device=/dev/video0 io-mode=4 ! queue ! video/x-raw,format=NV12,width=$WIDTH,height=$HEIGHT,framerate=$FPS/1 ! glimagesink


