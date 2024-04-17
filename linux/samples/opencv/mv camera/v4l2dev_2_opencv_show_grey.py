import sys
import argparse
import subprocess
import cv2

def read_cam(width, height, fps,i2c):
    
    v4l2_cmd = f'media-ctl -d /dev/media0 --set-v4l2 \'"m00_b_mvcam {i2c}-003b":0[fmt:Y8_1X8/{width}x{height}@1/{fps} field:none]\''
    subprocess.run(v4l2_cmd, shell=True)
    
    cap = cv2.VideoCapture(f"v4l2src io-mode=dmabuf device=/dev/video0 ! video/x-raw, format=(string)GRAY8, width=(int){width}, height=(int){height} ! appsink")
    if cap.isOpened():
        cv2.namedWindow("demo", cv2.WINDOW_AUTOSIZE)
        while True:
            ret_val, img = cap.read();
            cv2.imshow('demo',img)
            cv2.waitKey(1)
    else:
     print ("camera open failed");

    cv2.destroyAllWindows()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Read camera video stream')
    parser.add_argument('--width', type=int, default=1080, help='width of the video stream')
    parser.add_argument('--height', type=int, default=1080, help='height of the video stream')
    parser.add_argument('--fps', type=int, default=30, help='fps of the video stream')
    parser.add_argument('--i2c', type=int, default=7, help='i2c bus number')
    args = parser.parse_args()

    read_cam(args.width, args.height, args.fps, args.i2c)