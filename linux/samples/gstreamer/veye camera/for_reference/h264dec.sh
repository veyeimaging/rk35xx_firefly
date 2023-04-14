
#decode h264.mp4
#gst-launch-1.0 filesrc location=/usr/local/test.mp4 ! qtdemux ! h264parse ! mppvideodec ! rkximagesink

#with audio
gst-launch-1.0 filesrc location=/usr/local/test.mp4 ! queue ! qtdemux  name=dmux dmux.video_0 ! queue ! h264parse ! mppvideodec ! rkximagesink dmux.audio_0 ! queue ! aacparse ! faad ! autoaudiosink

#decode h264.ts encode by mppvideoenc
#gst-launch-1.0 uridecodebin uri=file:///home/firefly/h264.ts ! rkximagesink

