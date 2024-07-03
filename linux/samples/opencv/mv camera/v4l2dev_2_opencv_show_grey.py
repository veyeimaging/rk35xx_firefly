import sys
import argparse
import subprocess
import cv2
import time

def read_cam(width, height, fps,i2c):
    
    v4l2_cmd = f'media-ctl -d /dev/media0 --set-v4l2 \'"m00_b_mvcam {i2c}-003b":0[fmt:Y8_1X8/{width}x{height}@1/{fps} field:none]\''
    subprocess.run(v4l2_cmd, shell=True)
    
    cap = cv2.VideoCapture(f"v4l2src io-mode=dmabuf device=/dev/video0 ! video/x-raw, format=(string)GRAY8, width=(int){width}, height=(int){height} ! appsink")
    if cap.isOpened():
        
        cv2.namedWindow("demo", cv2.WINDOW_AUTOSIZE)

        start_time = time.time()
        frame_count = 0

        while True:
            ret_val, img = cap.read();
            frame_count += 1
        
            # calc framerate every 10 frames
            if frame_count >= 10:
                end_time = time.time()
                fps = frame_count / (end_time - start_time)
                frame_count = 0
                start_time = end_time
                
            #overlay FPS
            cv2.putText(img, f"FPS: {fps:.2f}", (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2, cv2.LINE_AA)
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