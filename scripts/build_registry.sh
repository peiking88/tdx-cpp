#!/bin/bash -e
# Build 完整 registry.cpp：Python 生成 246 信号注册表（含自修复）→ C++ 可编译代码
cd "$(dirname "$0")/.."
python3 scripts/gen_signal_registry.py
echo "Registry rebuilt"
