import socket
import struct
import numpy as np
from collections import defaultdict
import cv2

HEADER_FMT  = "=IHH"   # little-endian: frame_id, chunk_index, total_chunks
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 8 byte

WIDTH, HEIGHT = 256, 192

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 5000))
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)

buffers: dict[int, dict[int, bytes]] = defaultdict(dict)
last_frame = -1

print("In ascolto su :5000 ...")

while True:
    data, _ = sock.recvfrom(65535)
    if len(data) < HEADER_SIZE:
        continue

    frame_id, chunk_idx, total_chunks = struct.unpack_from(HEADER_FMT, data)
    payload = data[HEADER_SIZE:]

    if frame_id <= last_frame:
        continue  # frame già processato, scarta

    buffers[frame_id][chunk_idx] = payload

    if len(buffers[frame_id]) == total_chunks:
        raw = b"".join(buffers[frame_id][i] for i in range(total_chunks))

        # pulizia
        del buffers[frame_id]
        for old in [k for k in buffers if k < frame_id]:
            del buffers[old]
        last_frame = frame_id

        # BGRA → array numpy (192, 256, 4)
        frame = np.frombuffer(raw, dtype=np.uint8).reshape((HEIGHT, WIDTH, 4))

        # OpenCV vuole BGR
        bgr = cv2.cvtColor(frame, cv2.COLOR_BGRA2BGR)
        cv2.imshow("Bottom Screen", bgr)
        if cv2.waitKey(1) == 27:
            break