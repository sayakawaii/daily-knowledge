"""练习 5：YOLO 目标检测，自己写 letterbox 预处理、输出解码和 NMS

先准备引擎（见指南第 5 节）：
  yolo export model=yolo11n.pt format=onnx imgsz=640
  trtexec --onnx=yolo11n.onnx --saveEngine=yolo11n_fp16.plan --fp16
运行：python ex05_detect.py
需要练习 4 的 TrtModel 已经能用。
"""
import ast

import cv2
import numpy as np
import onnx

from ex04_tensorrt import TrtModel

INPUT_SIZE = 640
CONF_THRES = 0.25
IOU_THRES = 0.7


def load_names(onnx_path="yolo11n.onnx"):
    """Ultralytics 导出的 ONNX 里存了类别名，读出来。"""
    meta = {p.key: p.value for p in onnx.load(onnx_path, load_external_data=False).metadata_props}
    names = ast.literal_eval(meta["names"])
    return [names[i] for i in range(len(names))]


def letterbox(img_bgr, size=INPUT_SIZE):
    """等比缩放到长边 = size，剩下的地方用灰色 (114) 填满，图片放在正中间。
    返回 (处理后的图, 缩放比例 r, (左边填充的像素 pad_x, 上边填充的像素 pad_y))"""
    # 为什么不直接 resize 成 640x640？那样会把图片压扁，圆的变椭圆，模型训练时没见过这样的图。
    # TODO 1：h, w = img_bgr.shape[:2]；r = min(size / h, size / w)，保证长边刚好放进 size。
    # TODO 2：new_w, new_h = round(w * r), round(h * r)，用 cv2.resize 缩放（注意参数是 (宽, 高)）。
    # TODO 3：算四周要填多少。左右一共要填 size - new_w，上下一共要填 size - new_h，各分一半。
    #   为了和 Ultralytics 完全对齐：left = round(pad_x - 0.1)，top = round(pad_y - 0.1)，
    #   其中 pad_x = (size - new_w) / 2；right、bottom 用总数减去 left、top，避免差 1 个像素。
    # TODO 4：cv2.copyMakeBorder(resized, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
    # 返回 (图, r, (left, top))
    return None, None, None


def preprocess(img_bgr):
    lb, r, pad = letterbox(img_bgr)
    if lb is None:
        return None, None, None
    # 和分类不一样：YOLO 只要除以 255，不减均值、不除标准差。
    x = cv2.cvtColor(lb, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    x = np.ascontiguousarray(x.transpose(2, 0, 1)[np.newaxis])
    return x, r, pad


def box_iou(box, boxes):
    """一个框和一组框的 IoU（交并比）。框的格式是 [x1, y1, x2, y2]，boxes 形状 (N, 4)，返回 (N,)。"""
    # TODO 1：交集的左上角 = 两个左上角里更靠右下的那个：np.maximum(box[0], boxes[:, 0])，y1 同理。
    #   交集的右下角 = 两个右下角里更靠左上的那个：np.minimum(...)。
    # TODO 2：交集面积 = 宽 * 高，没有重叠时宽或高是负数，要用 np.clip(..., 0, None) 截成 0。
    # TODO 3：IoU = 交集 / (面积1 + 面积2 - 交集)。分母加一个 1e-9 防止除以 0。
    #   全程用数组运算，不要写 for 循环。
    return None


def nms(boxes, scores, iou_thres):
    """贪心 NMS：返回要保留的框的下标。"""
    # 同一个物体，模型会在附近报出好几个框。NMS 的做法是：
    # TODO 1：order = np.argsort(scores)[::-1]，按分数从高到低排。
    # TODO 2：while order.size > 0:
    #     取出分数最高的 i = order[0]，放进 keep；
    #     算它和剩下所有框 order[1:] 的 IoU；
    #     IoU >= iou_thres 的认为是重复的，扔掉；只保留 IoU < iou_thres 的，作为新的 order。
    return []


def postprocess(output, r, pad, orig_shape):
    """把模型输出 (1, 84, 8400) 变成检测结果：[(x1, y1, x2, y2, 置信度, 类别), ...]，坐标是原图上的。"""
    # 输出的含义：8400 个候选框，每个框 84 个数 = 4 个框坐标 (中心x, 中心y, 宽, 高) + 80 个类别的分数。
    # 这 80 个分数已经过 sigmoid，每个都在 0~1 之间、互相独立，所以这里不需要 softmax。
    # TODO 1：pred = output[0].T，变成 (8400, 84)，每行一个框，更好处理。
    # TODO 2：前 4 列是框，后 80 列是分数。np.argmax(axis=1) 得到每个框最可能的类别，
    #   再取出这个类别的分数作为置信度。
    # TODO 3：只保留置信度 > CONF_THRES 的框（用布尔数组 mask 筛选）。一个都没有就返回 []。
    # TODO 4：(cx, cy, w, h) 转成 (x1, y1, x2, y2)：x1 = cx - w/2 ...
    # TODO 5：按类别分别做 NMS。技巧：给每个框加一个偏移 类别号 * 7680，不同类别的框就离得很远，
    #   互相的 IoU 是 0，一次 nms 调用就等于分类别做。注意：偏移只用来算 NMS，不要改真正的框。
    # TODO 6：把坐标从 640x640 的输入图变回原图：先减去 pad，再除以 r；
    #   最后用 clip 截到原图范围内（x 在 0~宽，y 在 0~高）。
    # 返回 [(x1, y1, x2, y2, conf, cls), ...]，都转成 Python 的 float / int。
    return []


def draw(img, dets, names):
    for x1, y1, x2, y2, conf, cls in dets:
        cv2.rectangle(img, (int(x1), int(y1)), (int(x2), int(y2)), (0, 255, 0), 2)
        cv2.putText(img, f"{names[cls]} {conf:.2f}", (int(x1), max(int(y1) - 5, 15)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    return img


if __name__ == "__main__":
    names = load_names()
    model = TrtModel("yolo11n_fp16.plan")
    img = cv2.imread("assets/bus.jpg")
    assert img is not None, "找不到 assets/bus.jpg"

    x, r, pad = preprocess(img)
    if x is None or x.shape != (1, 3, INPUT_SIZE, INPUT_SIZE):
        print("[FAIL] letterbox 还没完成，输入不是 (1, 3, 640, 640)")
        raise SystemExit(1)
    out = next(iter(model.infer(x).values()))
    print("模型输出形状:", out.shape)
    dets = postprocess(out, r, pad, img.shape[:2])
    for d in dets:
        print(f"  {names[d[5]]:<10s} {d[4]:.2f}  [{d[0]:.0f}, {d[1]:.0f}, {d[2]:.0f}, {d[3]:.0f}]")
    cv2.imwrite("assets/bus_det.jpg", draw(img.copy(), dets, names))

    from ultralytics import YOLO
    ref = YOLO("yolo11n.pt").predict("assets/bus.jpg", conf=CONF_THRES, iou=IOU_THRES, verbose=False)[0]
    ref_boxes = ref.boxes.xyxy.cpu().numpy()
    ref_cls = ref.boxes.cls.cpu().numpy().astype(int)
    print(f"Ultralytics 官方结果: {len(ref_boxes)} 个框")

    matched = 0
    for d in dets:
        same = ref_cls == d[5]
        iou = box_iou(np.array(d[:4]), ref_boxes[same]) if same.any() else None
        if iou is not None and iou.max() > 0.9:
            matched += 1
    ok1 = out.shape == (1, 84, 8400)
    ok2 = abs(len(dets) - len(ref_boxes)) <= 1
    ok3 = len(dets) > 0 and matched >= len(dets) - 1
    print(f"[{'PASS' if ok1 else 'FAIL'}] 模型输出形状是 (1, 84, 8400)")
    print(f"[{'PASS' if ok2 else 'FAIL'}] 框的数量和官方差不超过 1 个（你: {len(dets)}，官方: {len(ref_boxes)}）")
    print(f"[{'PASS' if ok3 else 'FAIL'}] 你的框和官方的框基本重合（IoU > 0.9 的有 {matched} 个）")
    print("画好框的图保存在 assets/bus_det.jpg")
    print("\n全部通过！" if all([ok1, ok2, ok3]) else "\n还有没通过的，按注释提示继续改。")
