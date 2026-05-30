import cv2
for i in range(5):
    cap = cv2.VideoCapture(i, cv2.CAP_DSHOW)
    if cap.isOpened():
        ret, frame = cap.read()
        if ret:
            print(f"Camera index {i}: WORKS  (resolution {frame.shape[1]}x{frame.shape[0]})")
        else:
            print(f"Camera index {i}: opens but can't read")
        cap.release()
    else:
        print(f"Camera index {i}: not available")