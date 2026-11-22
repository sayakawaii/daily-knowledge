"""练习 4：用 TensorRT 的 Python 接口加载引擎并推理

先用 trtexec 构建引擎（见指南第 4 节）：
  trtexec --onnx=resnet18.onnx --saveEngine=resnet18_fp16.plan --fp16
运行：python ex04_tensorrt.py
这里写的 TrtModel 类，练习 5、6、7 都会用到。
"""
import os

import numpy as np
import tensorrt as trt
import torch

TRT_TO_TORCH = {
    trt.float32: torch.float32,
    trt.float16: torch.float16,
    trt.int32: torch.int32,
    trt.int8: torch.int8,
    trt.bool: torch.bool,
}


class TrtModel:
    def __init__(self, engine_path):
        self.logger = trt.Logger(trt.Logger.WARNING)
        with open(engine_path, "rb") as f, trt.Runtime(self.logger) as runtime:
            self.engine = runtime.deserialize_cuda_engine(f.read())
        assert self.engine is not None, f"加载引擎失败：{engine_path}（是不是在别的机器或别的 TensorRT 版本上构建的？）"
        self.context = self.engine.create_execution_context()
        # 这里借用 PyTorch 来申请显存和管理 CUDA 流，省得再装 pycuda / cuda-python。
        self.stream = torch.cuda.Stream()

        self.inputs, self.outputs = {}, {}
        # TODO：给引擎的每个输入、输出都准备一块显存，并告诉 TensorRT 地址在哪。
        #   for i in range(self.engine.num_io_tensors):
        #     1. name  = self.engine.get_tensor_name(i)
        #     2. shape = tuple(self.engine.get_tensor_shape(name))
        #     3. dtype = TRT_TO_TORCH[self.engine.get_tensor_dtype(name)]
        #     4. buf = torch.empty(shape, dtype=dtype, device="cuda")       在 GPU 上开一块空的
        #     5. self.context.set_tensor_address(name, buf.data_ptr())     把显存地址交给 TensorRT
        #     6. 用 self.engine.get_tensor_mode(name) == trt.TensorIOMode.INPUT 判断是输入还是输出，
        #        分别放进 self.inputs[name] 或 self.outputs[name]

    def infer(self, x):
        """x 是 numpy 数组，形状要和引擎的输入一致。返回 {输出名: numpy 数组}。"""
        # TODO：GPU 上的工作都是「排队」的：你发出命令，函数马上返回，GPU 稍后才真正执行。
        #   所以要放在同一个流里按顺序排好，最后等它全部做完。
        #   inp = next(iter(self.inputs.values()))
        #   with torch.cuda.stream(self.stream):
        #     1. inp.copy_(torch.from_numpy(x).to(inp.dtype), non_blocking=True)   把输入拷进显存
        #     2. self.context.execute_async_v3(self.stream.cuda_stream)            让 TensorRT 在这个流上推理
        #     3. 每个输出 buf.float().cpu()，拷回内存（FP16 的输出先转成 float32）
        #   self.stream.synchronize()   等 GPU 全部做完，再把结果 .numpy() 返回
        #   想一想：如果不 synchronize 就去读结果，可能读到什么？
        return None


if __name__ == "__main__":
    from ex01_numpy_softmax import softmax
    from ex02_classify_pytorch import LABELS, topk

    if not os.path.exists("assets/ref_logits_ort.npy"):
        print("[FAIL] 找不到 assets/ref_logits_ort.npy，先跑通练习 3")
        raise SystemExit(1)
    m = TrtModel("resnet18_fp16.plan")
    print("输入:", {k: (tuple(v.shape), v.dtype) for k, v in m.inputs.items()})
    print("输出:", {k: (tuple(v.shape), v.dtype) for k, v in m.outputs.items()})

    x = np.load("assets/ref_input.npy")
    ref = np.load("assets/ref_logits_ort.npy")
    out = m.infer(x)
    logits = None if not out else next(iter(out.values()))[0]
    if logits is None:
        print("[FAIL] 还没有输出")
    else:
        p, p_ref = softmax(logits), softmax(ref)
        t5, t5_ref = topk(p), topk(p_ref)
        diff = float(np.max(np.abs(p - p_ref)))
        ok1 = t5[0][0] == t5_ref[0][0]
        ok2 = len({i for i, _ in t5} & {i for i, _ in t5_ref}) >= 4
        ok3 = diff < 0.02
        for i, prob in t5:
            print(f"  {LABELS[i]:<25s} {prob:.4f}")
        print(f"[{'PASS' if ok1 else 'FAIL'}] 第 1 名和 ONNX Runtime 一致")
        print(f"[{'PASS' if ok2 else 'FAIL'}] 前 5 名至少有 4 个一致")
        print(f"[{'PASS' if ok3 else 'FAIL'}] 概率最大差 {diff:.4f}（FP16 会有一点误差，小于 0.02 算正常）")
        print("\n全部通过！" if all([ok1, ok2, ok3]) else "\n还有没通过的，按注释提示继续改。")
