#!/bin/bash -e
# Build 完整 registry.cpp：Python 生成 246 信号注册表 → C++ 可编译代码
cd /home/li/peiking88/czsc-cpp
python3 scripts/gen_signal_registry.py
# Fix F/S helper：inline lambda 代替 static helper
sed -i 's|static std::string S(const char\* s) { return std::string(s); }||
s|static std::string F(size_t n) { return std::to_string(n); }||' src/signals/registry.cpp
# Add inline lambda helpers before first use in bar stubs
sed -i '/\/\/ ---- bar: 单K\/多K形态识别/a\
auto F = [](size_t n) { return std::to_string(n); };\
auto S = [](const char* s) { return std::string(s); };' src/signals/registry.cpp
echo "Registry rebuilt"
