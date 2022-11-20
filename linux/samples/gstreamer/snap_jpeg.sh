#!/bin/bash

gst-launch-1.0 -v v4l2src device=/dev/video0 num-buffers=10 ! video/x-raw,format=UYVY,width=1920,height=1080 ! mppjpegenc ! multifilesink location=/tmp/test%05d.jpg
