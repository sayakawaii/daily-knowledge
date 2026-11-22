"""练习 3：把 PyTorch 模型导出成 ONNX，用 ONNX Runtime 推理，确认结果和 PyTorch 一样

运行：python ex03_onnx.py
需要先跑通练习 2（会生成 assets/ref_input.npy 和 assets/ref_logits_torch.npy）
"""
import os

import numpy as np
import onnx
import onnxruntime as ort
import torch

from ex02_classify_pytorch import LABELS, load_model, topk
from ex01_numpy_softmax import softmax

ONNX_PATH = "resnet18.onnx"

# 这块板子上 onnxruntime-gpu 的 CUDAExecutionProvider 没有为 Orin (sm_87) 编译，
# 一跑就报 cudaErrorNoKernelImageForDevice，所以 GPU 这一路用 TensorrtExecutionProvider。
PROVIDER_OPTIONS = {
    "TensorrtExecutionProvider": {"trt_engine_cache_enable": True, "trt_engine_cache_path": "ort_trt_cache"},
}
# CPU 用 FP32，和 PyTorch 几乎一样；TensorRT 默认允许 TF32（尾数只有 10 位的 FP32），误差会大一些。
TOLERANCE = {"CPUExecutionProvider": 1e-2, "TensorrtExecutionProvider": 5e-2}


def export_onnx(model, path):
    # TODO：用 torch.onnx.export 导出。它的工作方式是：喂一个假输入，跟着数据走一遍，把经过的运算记下来。
    #   1. 造一个假输入：torch.randn(1, 3, 224, 224, device="cuda")，形状要和真实输入一样。
    #   2. torch.onnx.export(model, (假输入,), path, ...)，需要的关键字参数：
    #        input_names=["input"], output_names=["logits"]  给输入输出起名字，后面 TensorRT 和 ORT 都按名字找
    #        opset_version=17                               ONNX 算子集版本
    #        dynamo=False                                   用老的导出方式（新方式需要 onnxscript，板子上没装）
    pass


def run_onnxruntime(path, x, provider):
    """用指定的 provider（CPU 或 TensorRT）跑一次推理，返回 1000 个 logits。"""
    # TODO 1：sess = ort.InferenceSession(path, providers=[(provider, PROVIDER_OPTIONS.get(provider, {}))])
    # TODO 2：输入名字不用写死，用 sess.get_inputs()[0].name 读出来。
    # TODO 3：sess.run(None, {输入名: x})。None 表示「所有输出都要」，返回的是一个 list，每个输出一项。
    #   取第一个输出，再取 [0] 去掉 batch 维度。
    return None


if __name__ == "__main__":
    if not os.path.exists("assets/ref_logits_torch.npy"):
        print("[FAIL] 找不到 assets/ref_logits_torch.npy，先跑通练习 2")
        raise SystemExit(1)
    model = load_model()
    export_onnx(model, ONNX_PATH)
    if not os.path.exists(ONNX_PATH):
        print(f"[FAIL] 没有生成 {ONNX_PATH}，先完成 export_onnx")
        raise SystemExit(1)
    m = onnx.load(ONNX_PATH)
    onnx.checker.check_model(m)
    print("ONNX 输入:", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
    print("ONNX 输出:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in m.graph.output])

    x = np.load("assets/ref_input.npy")
    ref = np.load("assets/ref_logits_torch.npy")
    results = []
    for provider in ["CPUExecutionProvider", "TensorrtExecutionProvider"]:
        out = run_onnxruntime(ONNX_PATH, x, provider)
        if out is None:
            print(f"[FAIL] {provider}: 还没有输出")
            results.append(False)
            continue
        diff = float(np.max(np.abs(out - ref)))
        top1 = topk(softmax(out), 1)[0]
        ok = diff < TOLERANCE[provider] and top1[0] == int(np.argmax(ref))
        print(f"[{'PASS' if ok else 'FAIL'}] {provider}: 和 PyTorch 的最大差 {diff:.2e}，第 1 名 {LABELS[top1[0]]} {top1[1]:.4f}")
        results.append(ok)
        if provider == "CPUExecutionProvider":
            np.save("assets/ref_logits_ort.npy", out)
    print("\n全部通过！" if all(results) else "\n还有没通过的，按注释提示继续改。")
