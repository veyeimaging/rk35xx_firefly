import numpy as np
import cv2 as cv
import os
import time

cap = cv.VideoCapture('v4l2src device=/dev/video0 ! video/x-raw, format=NV12, width=1920, height=1080, framerate=30/1 ! videoconvert ! appsink', cv.CAP_GSTREAMER)

if not cap.isOpened():
    print("Cannot capture from camera. Exiting.")
    os._exit()
last_time = time.time()

while(True):

    ret, frame = cap.read()
    this_time = time.time()
    print (str((this_time-last_time)*1000)+'ms')
    last_time = this_time;
    cv.imshow('frame', frame)

    if cv.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv.destroyAllWindows()
