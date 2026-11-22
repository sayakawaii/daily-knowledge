"""练习 1：NumPy 基础与 softmax

运行：python ex01_numpy_softmax.py
全部显示 PASS 就算完成。每个函数里的 TODO 是你要写的地方，写完把 `return None` 换成真正的结果。
"""
import numpy as np


def hwc_to_nchw(img):
    """把 OpenCV 读出来的图片 (H, W, C) 变成模型要的 (1, C, H, W)。"""
    # TODO 1：用 img.transpose(...) 把维度顺序从 (H, W, C) 换成 (C, H, W)。
    #   transpose 的参数是「新的第 0、1、2 维分别是原来的第几维」。
    # TODO 2：在最前面加一个 batch 维度，变成 (1, C, H, W)。
    #   提示：x[np.newaxis, ...] 或 np.expand_dims(x, 0)
    # TODO 3：transpose 不会真的搬数据，只是换了个看法。用 np.ascontiguousarray(x)
    #   让内存真正按新顺序排好，后面交给 GPU 的时候必须是连续的。
    return None


def normalize(img_rgb_uint8, mean, std):
    """把 0~255 的 uint8 图片变成 float32，先除以 255，再按通道减均值、除以标准差。"""
    # TODO 1：先 .astype(np.float32) 再除以 255.0。
    #   想一想：如果直接用 uint8 做减法会怎样？（uint8 没有负数）
    # TODO 2：(x - mean) / std。mean 和 std 的形状是 (3,)，图片是 (H, W, 3)，
    #   NumPy 会自动把 (3,) 对齐到最后一维，这叫「广播」(broadcasting)。
    # TODO 3：确认结果还是 float32（float32 和 float64 混算会变成 float64）。
    return None


def softmax(x, axis=-1):
    """数值稳定的 softmax。"""
    # 公式：softmax(x)_i = exp(x_i) / sum_j exp(x_j)
    # TODO 1：直接 np.exp(1000) 会溢出成 inf，inf / inf = nan。
    #   技巧：先减掉最大值。exp(x - max) / sum(exp(x - max)) 和原公式在数学上完全相等，
    #   但最大的那项变成 exp(0) = 1，不会溢出。
    #   用 np.max(x, axis=axis, keepdims=True)；keepdims=True 让结果能和 x 广播相减。
    # TODO 2：np.exp，再除以 np.sum(..., axis=axis, keepdims=True)。
    return None


def check(name, ok):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    return ok


if __name__ == "__main__":
    results = []

    img = np.random.randint(0, 256, size=(480, 640, 3), dtype=np.uint8)
    out = hwc_to_nchw(img)
    results.append(check("hwc_to_nchw 形状是 (1, 3, 480, 640)", out is not None and out.shape == (1, 3, 480, 640)))
    results.append(check("hwc_to_nchw 像素没有搬错位置", out is not None and out[0, 2, 10, 20] == img[10, 20, 2]))

    mean = np.array([0.485, 0.456, 0.406], dtype=np.float32)
    std = np.array([0.229, 0.224, 0.225], dtype=np.float32)
    n = normalize(img, mean, std)
    ref = (img[5, 7, 1] / 255.0 - mean[1]) / std[1]
    results.append(check("normalize 输出是 float32", n is not None and n.dtype == np.float32))
    results.append(check("normalize 数值正确", n is not None and abs(float(n[5, 7, 1]) - ref) < 1e-5))

    p = softmax(np.array([1.0, 2.0, 3.0]))
    results.append(check("softmax 结果加起来等于 1", p is not None and abs(float(np.sum(p)) - 1.0) < 1e-9))
    results.append(check("softmax 数值正确", p is not None and np.allclose(p, [0.09003057, 0.24472847, 0.66524096], atol=1e-6)))
    big = softmax(np.array([1000.0, 1001.0, 1002.0]))
    results.append(check("softmax 输入很大时不溢出（没有 nan）", big is not None and p is not None and not np.any(np.isnan(big)) and np.allclose(big, p, atol=1e-6)))
    m2 = softmax(np.array([[1.0, 2.0, 3.0], [0.0, 0.0, 0.0]]), axis=1)
    results.append(check("softmax 支持二维、按行计算", m2 is not None and np.allclose(m2.sum(axis=1), 1.0) and np.allclose(m2[1], 1 / 3)))

    print("\n全部通过！" if all(results) else "\n还有没通过的，按注释提示继续改。")
