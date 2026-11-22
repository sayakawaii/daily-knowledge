"""练习 7：接 USB 摄像头做实时检测，画面上显示帧率和各阶段耗时

运行：python ex07_camera.py            # 接了显示器：弹窗显示，按 q 退出
     python ex07_camera.py --save 10  # 通过 SSH 运行：录 10 秒，保存成 assets/live.mp4
需要练习 4、5 已通过。
"""
import argparse
import os
import time

import cv2

from ex04_tensorrt import TrtModel
from ex05_detect import draw, load_names, postprocess, preprocess


def open_camera(dev="/dev/video0", w=1280, h=720, fps=30):
    cap = cv2.VideoCapture(dev, cv2.CAP_V4L2)
    # 这个摄像头 1280x720 用 YUYV（不压缩）只有 10 fps，用 MJPG（摄像头里先压缩）才有 30 fps。
    # 不指定的话 OpenCV 可能选 YUYV，所以要明确设成 MJPG。
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, w)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, h)
    cap.set(cv2.CAP_PROP_FPS, fps)
    return cap


class FpsMeter:
    """用指数滑动平均算帧率，数字不会跳得太厉害。"""

    def __init__(self, alpha=0.1):
        self.alpha = alpha
        self.fps = None
        self.last = None

    def tick(self):
        """每处理完一帧调用一次，返回当前的平均帧率（第一次调用返回 None）。"""
        # TODO 1：now = time.perf_counter()
        # TODO 2：如果 self.last 不是 None：这一帧的瞬时帧率 inst = 1 / (now - self.last)
        #   如果 self.fps 还是 None，就直接等于 inst；
        #   否则 self.fps = (1 - alpha) * self.fps + alpha * inst
        #   （新值只占 10%，旧值占 90%，所以单帧的抖动会被抹平）
        # TODO 3：self.last = now，返回 self.fps
        return None


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--save", type=float, default=0, help="录多少秒并保存成视频；0 表示弹窗显示")
    args = ap.parse_args()

    names = load_names()
    model = TrtModel("yolo11n_fp16.plan")
    cap = open_camera()
    assert cap.isOpened(), "打不开摄像头，先用 v4l2-ctl --list-devices 看看设备号"
    show = args.save == 0 and os.environ.get("DISPLAY")
    writer = None
    meter = FpsMeter()
    t_start = time.perf_counter()
    frames, fps = 0, None
    lat_sum = {"cap": 0.0, "pre": 0.0, "infer": 0.0, "post": 0.0}

    while True:
        t0 = time.perf_counter()
        ok, frame = cap.read()
        if not ok:
            break
        t1 = time.perf_counter()
        x, r, pad = preprocess(frame)
        t2 = time.perf_counter()
        out = next(iter(model.infer(x).values()))
        t3 = time.perf_counter()
        dets = postprocess(out, r, pad, frame.shape[:2])
        t4 = time.perf_counter()

        fps = meter.tick()
        ms = {"cap": (t1 - t0) * 1e3, "pre": (t2 - t1) * 1e3, "infer": (t3 - t2) * 1e3, "post": (t4 - t3) * 1e3}
        for k in lat_sum:
            lat_sum[k] += ms[k]
        frames += 1

        draw(frame, dets, names)
        text = f"FPS {fps or 0:.1f} | cap {ms['cap']:.1f} pre {ms['pre']:.1f} infer {ms['infer']:.1f} post {ms['post']:.1f} ms"
        cv2.putText(frame, text, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

        if show:
            cv2.imshow("yolo11n TensorRT", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
        else:
            if writer is None:
                writer = cv2.VideoWriter("assets/live.mp4", cv2.VideoWriter_fourcc(*"mp4v"), 30, (frame.shape[1], frame.shape[0]))
            writer.write(frame)
            if time.perf_counter() - t_start > (args.save or 10):
                break

    cap.release()
    if writer:
        writer.release()
    if frames:
        real_fps = frames / (time.perf_counter() - t_start)
        avg = {k: v / frames for k, v in lat_sum.items()}
        print(f"共 {frames} 帧，平均帧率 {real_fps:.1f}")
        print("平均耗时（毫秒）: " + "  ".join(f"{k} {v:.1f}" for k, v in avg.items()))
        ok = fps is not None and abs(fps - real_fps) / real_fps < 0.3
        print(f"[{'PASS' if ok else 'FAIL'}] FpsMeter 算出来的帧率（{fps or 0:.1f}）和实际平均帧率对得上")
        print("想一想：cap 这一项为什么这么大？（提示：它包括等摄像头出下一帧，和 CPU 解码 MJPG）")
