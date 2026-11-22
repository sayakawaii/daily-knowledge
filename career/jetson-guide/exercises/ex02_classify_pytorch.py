"""练习 2：用 PyTorch 跑一个图像分类模型，自己写预处理和后处理

运行：python ex02_classify_pytorch.py
需要：assets/dog.jpg（见指南第 2 节的下载命令），练习 1 已通过
"""
import cv2
import numpy as np
import torch
from torchvision.models import ResNet18_Weights, resnet18

from ex01_numpy_softmax import hwc_to_nchw, normalize, softmax

MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)
LABELS = ResNet18_Weights.DEFAULT.meta["categories"]


def preprocess(img_bgr):
    """ResNet 的标准预处理：BGR 转 RGB，短边缩放到 256，中心裁剪 224x224，归一化，变成 (1, 3, 224, 224)。"""
    # TODO 1：OpenCV 读图是 BGR 顺序，模型训练时用的是 RGB。用 cv2.cvtColor(img, cv2.COLOR_BGR2RGB)。
    # TODO 2：短边缩放到 256，长边等比例缩放。
    #   h, w = img.shape[:2]；缩放倍数 s = 256 / min(h, w)
    #   cv2.resize 的尺寸参数是 (宽, 高)，不是 (高, 宽)！这是最常见的坑之一。
    #   用 interpolation=cv2.INTER_LINEAR。
    # TODO 3：从中间裁出 224x224。用切片 img[top:top + 224, left:left + 224]，
    #   top = (新高 - 224) // 2，left = (新宽 - 224) // 2。
    # TODO 4：调用练习 1 写的 normalize(img, MEAN, STD) 和 hwc_to_nchw(...)。
    return None


def topk(probs, k=5):
    """返回概率最大的 k 个类别：[(类别编号, 概率), ...]，按概率从大到小排。"""
    # TODO：np.argsort(probs) 返回「从小到大排序后的下标」。
    #   用 [::-1] 倒过来变成从大到小，再取前 k 个。
    #   返回时用 int(i) 和 float(probs[i]) 转成普通 Python 数字。
    return None


def load_model():
    model = resnet18(weights=ResNet18_Weights.DEFAULT)
    model.eval()
    return model.cuda()


def run_pytorch(model, x):
    """x 是 numpy 数组 (1, 3, 224, 224)。返回 1000 个 logits（还没有 softmax 的原始分数），numpy 数组。"""
    # TODO 1：torch.from_numpy(x) 把 numpy 变成 tensor，.cuda() 搬到 GPU。
    # TODO 2：在 `with torch.no_grad():` 里调用 model(...)。
    #   推理不需要算梯度，no_grad 能省显存、更快。
    # TODO 3：结果形状是 (1, 1000)，在 GPU 上。.cpu().numpy() 拿回来，再取 [0] 去掉 batch 维度。
    return None


if __name__ == "__main__":
    img = cv2.imread("assets/dog.jpg")
    assert img is not None, "找不到 assets/dog.jpg，先按指南下载"
    model = load_model()

    from PIL import Image
    ref_x = ResNet18_Weights.DEFAULT.transforms()(Image.open("assets/dog.jpg").convert("RGB")).unsqueeze(0).numpy()

    x = preprocess(img)
    print("输入形状:", None if x is None else x.shape, None if x is None else x.dtype)
    ok1 = x is not None and x.shape == (1, 3, 224, 224) and x.dtype == np.float32
    logits = run_pytorch(model, x if ok1 else ref_x)
    if logits is None:
        print("[FAIL] run_pytorch 还没有返回结果")
        raise SystemExit(1)
    ref_logits = run_pytorch(model, ref_x)
    probs, ref_probs = softmax(logits), softmax(ref_logits)
    top5, ref_top5 = topk(probs), topk(ref_probs)
    if top5 is None:
        print("[FAIL] topk 还没有返回结果")
        raise SystemExit(1)
    for i, p in top5:
        print(f"  {LABELS[i]:<25s} {p:.4f}")

    ok2 = ok1 and top5[0][0] == ref_top5[0][0]
    ok3 = ok1 and len({i for i, _ in top5} & {i for i, _ in ref_top5}) >= 4
    ok4 = ok1 and abs(top5[0][1] - ref_top5[0][1]) < 0.05
    print(f"[{'PASS' if ok1 else 'FAIL'}] 输入形状和类型正确")
    print(f"[{'PASS' if ok2 else 'FAIL'}] 第 1 名和官方预处理的结果一致（官方: {LABELS[ref_top5[0][0]]}）")
    print(f"[{'PASS' if ok3 else 'FAIL'}] 前 5 名至少有 4 个和官方一致")
    print(f"[{'PASS' if ok4 else 'FAIL'}] 第 1 名的概率和官方相差小于 0.05（官方: {ref_top5[0][1]:.4f}）")
    np.save("assets/ref_input.npy", x if ok1 else ref_x)
    np.save("assets/ref_logits_torch.npy", logits)
    print("\n全部通过！" if all([ok1, ok2, ok3, ok4]) else "\n还有没通过的，按注释提示继续改。")
