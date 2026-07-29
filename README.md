# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、TDengine 存储导入、断点续传。

## 快速开始

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

## 辅助脚本

```bash
python3 scripts/smmd.py --zxg   # SMMD 股市操纵检测（K-Means++ 聚类 + 启发式欺诈分类）
python3 scripts/append.py       # 全市场增量补导（读 stock_name → vipdoc + 网络增量入库）
```