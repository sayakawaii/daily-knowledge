"""练习 6：正确地测延迟，分清「纯推理」和「端到端」

运行前：sudo nvpmodel -m 2 && sudo jetson_clocks
运行：python ex06_benchmark.py
需要练习 4、5 已通过。
"""
import time

import cv2
import numpy as np

from ex04_tensorrt import TrtModel
from ex05_detect import postprocess, preprocess


def measure(fn, warmup=20, iters=200):
    """先空跑 warmup 次（不计时），再计时 iters 次。返回每次耗时（毫秒）的列表。"""
    # TODO 1：为什么要预热？头几次运行要做显存分配、CUDA 初始化、缓存加载，会特别慢，
    #   算进去会把平均值拉高。先调用 fn() warmup 次，不计时。
    # TODO 2：再循环 iters 次，每次用 time.perf_counter() 在 fn() 前后各取一次时间，
    #   差值乘 1000 变成毫秒，append 到列表里。
    #   不要用 time.time()，它精度低，还会被系统改时间影响。
    #   注意：GPU 是异步的，计时一定要包含「等 GPU 做完」。练习 4 的 infer 里已经 synchronize 了，
    #   所以这里直接计时是准的。如果 infer 没有 synchronize，你测到的只是「发命令」的时间。
    return None


def stats(times):
    """返回 (平均值, 中位数 p50, p90, p99)，单位毫秒。"""
    # TODO：a = np.array(times)，平均值用 a.mean()，百分位数用 np.percentile(a, 50) 这样算。
    #   为什么要看 p99？平均 6 ms 但偶尔卡 30 ms，对实时视频来说就是掉帧，平均值看不出来。
    return None


if __name__ == "__main__":
    model = TrtModel("yolo11n_fp16.plan")
    img = cv2.imread("assets/bus.jpg")
    x, r, pad = preprocess(img)
    out = next(iter(model.infer(x).values()))

    def end_to_end():
        x, r, pad = preprocess(img)
        o = next(iter(model.infer(x).values()))
        return postprocess(o, r, pad, img.shape[:2])

    rows = [
        ("前处理 preprocess", lambda: preprocess(img)),
        ("推理 infer（含拷入拷出）", lambda: model.infer(x)),
        ("后处理 postprocess", lambda: postprocess(out, r, pad, img.shape[:2])),
        ("端到端 end-to-end", end_to_end),
    ]
    print(f"{'阶段':<26s}{'平均':>8s}{'p50':>8s}{'p90':>8s}{'p99':>8s}   (毫秒)")
    results = {}
    for name, fn in rows:
        t = measure(fn)
        if t is None or len(t) == 0:
            print(f"{name:<26s} 还没有结果")
            continue
        s = stats(t)
        if s is None:
            print(f"{name:<26s} stats 还没写")
            continue
        results[name] = s
        print(f"{name:<26s}{s[0]:8.2f}{s[1]:8.2f}{s[2]:8.2f}{s[3]:8.2f}")

    ok = len(results) == 4
    if ok:
        parts = sum(results[k][1] for k in list(results)[:3])
        e2e = results["端到端 end-to-end"][1]
        ok = abs(parts - e2e) / e2e < 0.3
        print(f"\n三段中位数相加 {parts:.2f} ms，端到端中位数 {e2e:.2f} ms")
    print(f"[{'PASS' if ok else 'FAIL'}] 四个阶段都测出来了，而且三段相加和端到端对得上（差不超过 30%）")
    print("把「推理」这一行和 trtexec 报的 GPU Compute Time 比一比，想想为什么不一样。")
