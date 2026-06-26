# tdx-cpp

通达信行情数据 C++17 单库——TCP 协议帧编解码、A股/期货/港美股行情解析（K线/分时/逐笔/五档）、本地 vipdoc 读取、复权因子自算、断点续传、DuckDB Parquet 落盘、TDengine 导入。构建依赖 C++17 编译器、CMake、Ninja、Boost(context+system)、OpenSSL。

```bash
bash scripts/setup_external.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bin/tdx bars 600000 4 10
```
