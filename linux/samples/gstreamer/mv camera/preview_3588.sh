#!/bin/sh

#export DISPLAY=:0.0
#export GST_DEBUG=*:5
#export GST_DEBUG_FILE=/tmp/2.txt

#export XDG_RUNTIME_DIR=/run/user/1000

export WIDTH=1080
export HEIGHT=1024
export FPS=30
export I2C_BUS=7
echo "Just use 1080x1024@30fps,I2C bus is 7 as a sample. Please modify these value."

echo "Start MV Camera Preview!"

v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl roi_x=0

v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl roi_y=0

media-ctl -d /dev/media0 --set-v4l2 '"m00_b_mvcam '"$I2C_BUS"'-003b":0[fmt:Y8_1X8/'"$WIDTH"'x'"$HEIGHT"'@1/'"$FPS"']'

gst-launch-1.0 v4l2src device=/dev/video0 ! video/x-raw,format=GRAY8,width=$WIDTH,height=$HEIGHT ! autovideosink



