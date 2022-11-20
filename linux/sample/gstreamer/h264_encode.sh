#!/bin/bash

gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=100 !  video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! videoconvert ! mpph264enc ! h264parse ! mp4mux !  filesink location=./h264.mp4
