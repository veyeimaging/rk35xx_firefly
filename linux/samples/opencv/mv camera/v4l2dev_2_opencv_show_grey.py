import cv2
import argparse
import subprocess

def main():
    # Set up command-line argument parser
    parser = argparse.ArgumentParser(description='Real-time display of GREY image from /dev/video0')
    parser.add_argument('--width', type=int, default=640, help='image width (default: 640)')
    parser.add_argument('--height', type=int, default=480, help='image height (default: 480)')
    parser.add_argument('--fps', type=int, default=30, help='frame rate (default: 30)')
    args = parser.parse_args()
    
    v4l2_cmd = f'media-ctl -d /dev/media0 --set-v4l2 \'"m00_b_mvcam 7-003b":0[fmt:Y8_1X8/640x480@100/3000 field:none]\''
    subprocess.run(v4l2_cmd, shell=True)
    
    # Open the /dev/video0 device
    cap = cv2.VideoCapture('/dev/video0')
    if not cap.isOpened():
        print("Failed to open video device")
        return

    # Set the image size
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)

    # Loop over frames and display them
    while True:
        # Read a frame
        ret, frame = cap.read()

        # Check if reading was successful
        if not ret:
            print("Failed to read frame")
            break

        # Display the frame
        cv2.imshow('VEYE MV camera GREY image preview', frame)

        # Exit if 'q' key is pressed
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    # Release resources
    cap.release()
    cv2.destroyAllWindows()

if __name__ == '__main__':
    main()
