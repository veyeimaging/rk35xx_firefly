

#gst-launch-1.0 videotestsrc num-buffers=512 ! video/x-raw,format=NV12,width=640,height=480,framerate=30/1 ! queue ! mpph264enc ! queue ! h264parse ! mpegtsmux ! filesink location=/home/firefly/h264.ts
#gst-launch-1.0 uridecodebin uri=file:///home/firefly/h264.ts ! rkximagesink

#gst-launch-1.0 v4l2src device=/dev/video1 ! videoconvert ! video/x-raw,format=NV12,width=640,height=480 ! queue ! mpph264enc ! queue ! h264parse ! mpegtsmux ! filesink location=/home/firefly/h264.ts
#gst-launch-1.0 uridecodebin uri=file:///home/firefly/h264.ts ! rkximagesink

#gst-launch-1.0 v4l2src device=/dev/video1 ! videoconvert ! video/x-raw,format=NV12,width=640,height=480 ! queue ! kmssink

#gst-launch-1.0 v4l2src device=/dev/video1 ! videoconvert ! video/x-raw,format=RGB,width=640,height=480 ! queue ! rgaconvert hflip=false vflip=false rotation=90 input-crop=0x0x640x480 output-crop=0x0x320x240 output-io-mode=dmabuf capture-io-mode=dmabuf ! "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1" ! kmssink

#gst-launch-1.0 v4l2src device=/dev/video1 ! queue ! rgaconvert hflip=false vflip=false rotation=90 input-crop=0x0x640x480 output-crop=0x0x320x240 output-io-mode=dmabuf capture-io-mode=dmabuf ! "video/x-raw,format=NV12,width=320,height=240,framerate=30/1" ! queue ! kmssink

#gst-launch-1.0  filesrc location=/usr/local/test.mp4 ! qtdemux ! queue ! h264parse ! mppvideodec ! rgaconvert output-io-mode=dmabuf-import capture-io-mode=dmabuf vpu-stride=true ! "video/x-raw,format=NV12, width=1920,height=1080"  ! rkximagesink
