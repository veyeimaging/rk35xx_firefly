#!/bin/bash

gst-launch-1.0 v4l2src device=/dev/video0 num-buffers=100 !  video/x-raw,format=NV16,width=1920,height=1080,framerate=30/1 ! queue! videoconvert ! mpph264enc ! queue ! h264parse ! mp4mux !  filesink location=./h264.mp4
