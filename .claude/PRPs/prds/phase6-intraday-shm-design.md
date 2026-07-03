# Phase 6 — 盘中实时行情共享内存（获取-缓存-异步入库）

> 把 `tdx fetch-quotes` 从「获取-同步入库」改造为「获取-缓存(mmap)-异步入库」，并以裸 mmap 共享内存支持多个独立 C++ 分析进程（Krono / czSC 等）零拷贝低延迟读取盘中最新行情。

*Generated: 2026-07-03*
*Status: v0.3 — 二审 P1-new（head 写完推进）+ P2-1/P2-4 已修复，可进 Phase 6.1 MVP*
*关联：`tdx-cpp.prd.md`（总 PRD）、`src/cli/fetch_quotes.cpp`（现状采集器）*
*版本目标：v0.13.0*

---

## 一、Problem Statement（背景与动机）

现状（经 `src/cli/fetch_quotes.cpp` 实读确认）：`fetch-quotes` 是**单进程采集器**，主线程跑 `do/while` 轮询循环（`fetch_quotes.cpp:835-864`），每轮：

1. 新建 helio ProactorPool，N 个 fiber worker 并发拉取通达信（`RunOneRound` 阶段A，`:681-711`）；
2. worker 把数据 move 进共享 `chunks`；
3. **主线程顺序遍历 chunks，同步 `taos_query` 批量 INSERT 到 TDengine**（阶段B，`:713-774`）；
4. 等整批 INSERT 完成后才进入下一轮 fetch。

由此产生三个痛点：

| # | 痛点 | 根因 |
|---|---|---|
| 1 | **采集被入库阻塞** | 阶段B 在主线程同步执行，TDengine INSERT 慢直接拖慢下一轮 fetch 的节拍 |
| 2 | **无全市场最新价快照** | 进程内不留任何「最新价」视图，数据存活仅一轮 fetch→INSERT（秒级） |
| 3 | **分析进程只能查 TDengine** | Krono / czSC 等独立进程读「最新价」的唯一途径是 `SELECT ... ORDER BY ts DESC LIMIT 1`，连接重、延迟高，无法支撑高频轮询全市场 |

目标架构：

```
┌─────────────── 采集进程 fetch-quotes ───────────────────┐
│  fiber 池拉取(不变) → 主线程聚合(不变)                    │
│        ├─ 同步写 → mmap 共享内存 [快照表 + 时序 ring×3]   │ ← 单写者, seqlock
│        └─ 投递  → 内存队列(SPSC, 有界)                    │
│                        │                                  │
│              独立入库线程 std::thread                     │ ← drain + 批写 TDengine
│                        ↓                                   │   (taos_* 阻塞, 脱离 helio)
│                   TDengine (持久层 / 历史回溯)             │
└──────────────────────────────────────────────────────────┘
            ↑ mmap 只读挂载（MAP_SHARED, PROT_READ）
   ┌────────┴────────┬──────────────┐
   │ Krono 分析进程   │ czSC 分析进程 │  ... (均为 C++ 进程)
   └─────────────────┴──────────────┘
   读者无锁读（seqlock 版本号重试 / ring cursor 推进），多读者互不阻塞
```

**收益**：采集节拍与 TDengine 解耦（解痛 1）；进程内/跨进程共享同一份全市场最新价快照（解痛 2、3）；分析进程 O(1) 槽位定位读最新价，绕开 TDengine 连接开销。

---

## 二、Evidence（事实依据）

### 2.1 数据结构全部天然定长 POD（关键前提）

实读 `include/tdx/types.hpp`，待入 mmap 的结构体定长性：

| 结构 | 字段构成 | sizeof | 定长 POD？ |
|---|---|---|---|
| `Tick` | datetime(8)+price(8)+avg(8)+volume(8) | **32B** | ✅ |
| `Transaction` | datetime(8)+price(8)+volume(8)+trans_id(8)+buy_sell(1)+pad(7) | **40B** | ✅ |
| `KLine` | datetime(8)+open/close/high/low/volume/amount(6×8)+up/down_count(2×4) | **64B** | ✅ |
| `HistoryOrder` | price(8)+unknown(8)+vol(8) | **24B** | ✅ |
| `HistoryTransaction` | minutes(4)+price(8)+vol(8)+buy_sell(4)+pad(4) | **24B** | ✅ |
| `Quote` | 含 `std::string code/name` | ~280B | ⚠️ 见下 |

`Quote` 含两个 `std::string`，但 intraday 路径上 `parsers_quotes.cpp:90` **只填 `code`、`name` 恒为空**，且 `code` 是 6 字节股票代码 → POD 化为 `char code[8]` 即可，数据体 = datetime(8)+7×double(56)+4×5×double(160) = **224B**。**所有结构均可无损映射为定长 POD**，mmap 布局无需变长字符串处理。

### 2.2 现状代码位点（改造锚点）

| 位点 | 现状 | 改造 |
|---|---|---|
| `fetch_quotes.cpp:713-774` 阶段B | 主线程同步 INSERT | 拆为「写 mmap + 投递队列」，INSERT 移走 |
| `fetch_quotes.cpp:478` 注释 | 「fiber 内不触 TDengine（taos_* 阻塞 + 与 Proactor 冲突会堆损坏）」 | 异步入库线程必须脱离 helio Proactor，印证 D4 |
| `fetch_quotes.cpp:697` `fb2::Mutex mu` | 仅保护 chunks 聚合 | 保持；mmap 写在主线程聚合后，无需额外锁 |
| `fetch_quotes.cpp:835-864` 主线程轮询 | `std::this_thread::sleep_for`（主线程，合规） | 不变 |

### 2.3 环境与协议约束

- `/dev/shm` tmpfs：**45G 可用**，足以承载共享段（见 §4 估算 ~80MB）。
- 协议层是 **poll 模型**（通达信无 push），实时性上限 = `--quote_interval`（默认 30s）。mmap 不提升更新频率，只让读侧绕过 TDengine。
- 全 A 股 + ETF/指数/板块 ≈ **8000 量级**（快照槽数取 8192 = 2¹³）。

---

## 三、关键决策（ADR 核心）

### D1：段格式 —— 裸 mmap + 定长 POD，**不**用 Boost.Interprocess

**决策**：用 `<sys/mman.h>` 裸映射 + 自描述段头 + 定长 POD 槽，不引入 `managed_mapped_file` / `multi_index` / `bip::basic_string`。

**理由**（即便用户确认分析进程「全部 C++」，仍选裸 mmap）：
1. **无新依赖**：项目栈是 helio + abseil + iconv + zlib + TDengine，引入 Boost.Interprocess + MultiIndex + Date_Time 是全新重量级依赖，与「简单优先」相悖。
2. **helio 纪律**：Boost.Interprocess 的 `interprocess_mutex` / `interprocess_condition` 是**阻塞原语**，fiber 内禁用（CLAUDE.md「helio fiber 编码纪律」）。裸 mmap 配 `std::atomic` / seqlock 不涉及阻塞，fiber 安全。
3. **性能**：定长槽 O(1) 哈希定位 + 原子版本号，优于 Boost 托管段内的红黑树（`multi_index`）+ 段内动态分配（`basic_string`）。
4. **可控**：自描述段头（magic/version/generation）让崩溃恢复与多版本兼容可自行设计；Boost 托管段的内部簿记是黑盒。

**反证（已排除的替代）**：`/home/li/下载/mmap-container` 草稿即 Boost 托管段方案——粗粒度 `named_mutex`（系统调用 + 读写互斥）、读时持锁整条拷贝、无 seqlock、无逐笔流结构、`ptime` 非平凡类型不宜放 mmap、无 LICENSE、main 空壳。**仅作概念参考，不采纳**。

### D2：共享内存放 `/dev/shm`（tmpfs），**不** file-backed 持久化

**决策**：段文件路径默认 `/dev/shm/tdx_intraday.shm`（可经 `--mmap-path` / 环境变量配置）。

**理由**：盘中实时快照的语义是「最新即有效」，**无需跨重启持久化**（重启后采集进程重建即可）；tmpfs 零磁盘 IO；45G 充足。file-backed（如 mmap-container 的 `./stock_quotes.db`）对实时行情是负优化（引入写盘脏页回写）。

### D3：mmap 写者 = 采集主线程（单写者），seqlock 单写者协议

**决策**：mmap 的所有写入集中在 `fetch_quotes.cpp` 阶段B 的主线程聚合点（fiber worker **不直接写 mmap**）。并发协议用**单写者 seqlock**。

**理由**：现状本就是「fiber 拉取 → 主线程聚合」，在原 `InsertQuote` 同点位双写 mmap 即可，写者天然单一。seqlock 单写者版本最简单：写者 `seq` 偶→奇（fence）→写载荷→奇→偶（fence）；读者读 `seq`→（奇或变化则重试）。**读者完全无锁、不阻塞写者**，完美匹配「采集每 30s 写 + 多分析进程高频读」。

### D4：异步入库 —— 独立 `std::thread`，脱离 helio Proactor

**决策**：新增 `IngestWorker` 独立线程（`std::thread`，非 fiber），消费一个**有界 SPSC 队列**（主线程单生产者 → 入库线程单消费者），批量 drain 后 `taos_query` 写 TDengine。

**理由**：
- `taos_*` 阻塞（`fetch_quotes.cpp:478` 注释已点明与 Proactor 冲突致堆损坏），**必须脱离 helio Proactor**；独立 `std::thread` 不在 Proactor 线程，可用 `std::mutex`/`condition_variable`（与现状主线程 `sleep_for` 同属 fiber 纪律豁免）。
- SPSC 无锁单生产单消费，主线程投递零开销。
- **背压**：队列满时**覆盖最旧**（盘中数据「最新优先」，历史落库已由 TDengine 兜底）。

### D5：多读者无锁，写者存活靠 heartbeat + generation 检测

**决策**：分析进程 `mmap(MAP_SHARED, PROT_READ)` 只读挂载；读快照用 seqlock 重试，读 ring 各自维护进程内消费 cursor。段头维护 `writer_heartbeat_epoch`（写者每轮更新）与 `generation`（写者启动 +1），读者据此判断「写者是否存活 / 段是否被重建」。

**理由**：跨进程读不能用 `util::fb2::Mutex`（非进程间）；seqlock + 原子版本号跨进程可行，但依赖两条 **P0 invariant**（详见 §五 5.0、5.4）：

1. **原子性前提**：所有共享内存原子字段必须 `is_always_lock_free`（编译期断言）。`std::atomic` 标准仅保证单进程多线程语义，跨进程靠平台 lock-free 指令（x86 `LOCK` 前缀 / ARM `LDAR·STLR`）；非 lock-free 会退化为进程内锁，跨进程**静默失效**。
2. **单写者前提**：seqlock 是单写者算法，多写者会静默数据损坏。段头 `writer_pid` + flock 全周期持有强制唯一写者。

### D6：三种数据形态分区，统一段头

**决策**：段内分四区——段头 + 快照表（Quote upsert）+ 三条时序 ring（逐笔 Transaction / 委托 HistoryOrder / 增量分钟K KLine）。Tick（分时）按需补 ring。

**理由**：快照是「按 code 覆盖最新」，用定长槽 + 哈希；时序是「追加 + 读增量」，用 ring。两者访问模式不同，不可混用同一种结构。

---

## 四、段布局（字节图）

```
偏移         区                  说明
─────────────────────────────────────────────────────────────────
0x0000       SegmentHeader       ~160B（见下）
snap_off     SnapshotTable       snap_slots × 256B  （Quote 快照）
txn_off      RingHeader + slots  Transaction 逐笔流
ord_off      RingHeader + slots  HistoryOrder 委托流
kmin_off     RingHeader + slots  KLine 增量分钟K
kmin_end     (段尾)
```
> 各区偏移由 `SegmentHeader` 的 `snap_off/txn_off/ord_off/kmin_off` 字段自描述，读者据此定位，不写死。

### SegmentHeader（约 160B）

```cpp
struct SegmentHeader {
  char     magic[8];                 // "TDXSHM\0"
  uint32_t version;                  // 布局版本 = 1
  uint32_t header_size;              // = sizeof(SegmentHeader)
  uint64_t generation;               // 写者每次启动 +1（读者判重建）
  uint64_t capacity_bytes;           // 段总大小
  // 写者存活
  std::atomic<uint64_t> writer_heartbeat_epoch;  // 写者每轮 fetch 更新
  uint64_t writer_pid;
  uint64_t create_epoch;
  // 各区描述
  uint32_t snap_slots;               // 8192
  uint32_t snap_slot_size;           // 256
  uint64_t snap_off;                 // = header_size
  uint64_t txn_off;  uint64_t txn_capacity;  uint64_t txn_slot_size;  // 48
  uint64_t ord_off;  uint64_t ord_capacity;  uint64_t ord_slot_size;  // 32
  uint64_t kmin_off; uint64_t kmin_capacity; uint64_t kmin_slot_size; // 72
  char     reserved[16];             // 预留，全 0
};
```

### SnapSlot（256B，`alignas(64)`）—— Quote 快照

```cpp
struct SnapSlot {
  std::atomic<uint64_t> seq;         // seqlock：偶=稳定，奇=写中
  char     code[8];                  // "600000"，全 0 = 空槽
  uint64_t datetime;                 // epoch seconds
  double   price, pre_close, open, high, low, volume, amount;   // 7×8
  double   bid[5], ask[5], bid_vol[5], ask_vol[5];              // 20×8
  uint32_t flags;                    // bit0=有效；NaN 哨兵沿用 types.hpp 约定
  char     pad[12];                  // 凑 256B
};
// sizeof = 8(seq)+8(code)+8(datetime)+56(7×double)+160(20×double)+4(flags)+12(pad) = 256B
```

### 时序 ring（统一布局）

```cpp
struct RingHeader {
  std::atomic<uint64_t> head;        // 已写完可读位置（写者写完槽后才 store 推进；读者据此知可读到哪）
  uint64_t capacity;                 // 槽数，2 的幂
  uint64_t slot_size;                // = sizeof(seq) + sizeof(PayloadPOD)
  std::atomic<uint64_t> total_pushed;// 累计推送数（诊断覆盖丢失，§7 6.4 metrics）
  char     pad[32];                  // 凑 64B，head 独占 cache line
};
// 注：可读性靠 per-slot seq 判断，不另设 committed（评审 P1-1：删除死字段）
//
// 槽：{ std::atomic<uint64_t> seq;  PayloadPOD payload; }
// seq 编码（评审 P1-2 修复）：奇=写中，偶=完成；round = seq/2
//   写前  seq = 2*round + 1   （奇：占位，读者见此重试本槽）
//   写完  seq = 2*round + 2   （偶：可读；读者据此 + round 匹配判定）
```

### 布局不变量（编译期校验，评审 P0-1 修复）

```cpp
#include <atomic>
#include <cstddef>

// 共享内存原子字段必须 lock-free（标准仅保证单进程语义，跨进程靠平台原子指令）
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "shm 原子字段必须 lock-free，否则跨进程静默失效");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "...");

// 关键字段对齐：seq / head 必须自然对齐且起始于 cache line
static_assert(alignof(SnapSlot) == 64 && offsetof(SnapSlot, seq) == 0,
              "SnapSlot 需 64B 对齐，seq 起始偏移 0");
static_assert(alignof(RingHeader) == 64 && offsetof(RingHeader, head) == 0,
              "RingHeader 需 64B 对齐，head 起始偏移 0");
static_assert(sizeof(SnapSlot) == 256, "SnapSlot 定长 256B");
// 目标平台：x86-64 / ARM64（uint64_t is_always_lock_free == true）
// 备选实现：改用 GCC __atomic_* builtins，语义更明确（明确 memory order / 无锁）
```

### 容量估算（初值，均可配置）

| 区 | 槽数 | 槽大小 | 小计 |
|---|---|---|---|
| 快照表 | 8192 | 256B | 2 MB |
| 逐笔 ring | 2²⁰ ≈ 100 万 | 48B | 48 MB |
| 委托 ring | 2¹⁸ ≈ 26 万 | 32B | 8 MB |
| 分钟K ring | 2¹⁸ ≈ 26 万 | 72B | 18 MB |
| **合计** | | | **~76 MB**（/dev/shm 45G，余量充足） |

> 覆盖窗口：逐笔 100 万槽 ÷ 盘中峰值流量（~1万只 × 数笔/秒）≈ 分钟级回溯；分析进程若消费慢被覆盖，按 ring 语义跳过旧数据读最新。

---

## 五、并发协议

### 5.0 跨进程原子性与单写者保障（P0 invariant，评审 P0-1/P0-2 修复）

本设计所有并发正确性建立在两条 P0 invariant 上，**违反则静默数据损坏（非崩溃）**。

#### 5.0.1 原子性前提：共享内存原子字段必须 lock-free

`std::atomic` 标准仅保证**单进程多线程**语义；跨进程（不同虚拟地址映射同一 `MAP_SHARED` 物理页）的语义标准未定义，实践中依赖「lock-free 原子用 CPU 总线级指令（x86 `LOCK` 前缀 / ARM `LDAR·STLR`），与进程无关」这一平台事实。若某字段在目标平台 `!is_always_lock_free`，`std::atomic` 退化为进程内锁，跨进程完全失效。

强制约束（编译期断言见 §四「布局不变量」）：

- 所有共享内存原子类型 `static_assert(...::is_always_lock_free)`。
- `SnapSlot` / `RingHeader` `alignas(64)` + `offsetof` 校验关键字段对齐。
- 目标平台 x86-64 / ARM64（`uint64_t` 的 `is_always_lock_free` 均为 true）。
- **MVP 须含双进程原子性实测**：父进程写、`fork()` 子进程读，长时压验证无撕裂。
- 备选实现：改用 GCC `__atomic_load_n / __atomic_store_n / __atomic_thread_fence` builtins（明确 memory order、明确无锁），避开 `std::atomic` 成员函数可能的非平凡布局。

#### 5.0.2 单写者前提：seqlock 只允许单一写者

seqlock 是单写者算法——两个写者各自 `seq+1 → 写载荷 → seq+1` 会互相穿插，奇偶语义崩塌，读者读撕裂数据。**mmap 写 API（`Snapshot::Put` / `Ring::Push`）只能由单一进程的单一线程（采集主线程）调用**，违反导致静默数据损坏。

运行期保障（把「唯一写者」从约定升级为强制）：

- 段头 `writer_pid`：写者启动时写入自身 PID；新写者启动前 `kill(writer_pid, 0)` 探活，**旧 PID 仍存活 → abort 并报错**（不静默接管）。
- `flock(fd, LOCK_EX | LOCK_NB)`：写者全生命周期持有段文件排他锁，拿不到即退出。
- 多采集进程并发写**不在本设计范围**（须改多写者协议：per-writer seq 或 `pthread_mutex` + `PTHREAD_PROCESS_SHARED`，不能复用单写者 seqlock）。

### 5.1 快照表（seqlock 单写多读）

```
// 写者（采集主线程）
void put(const char* code, const QuotePOD& q) {
  SnapSlot& s = slot_of(code);            // FNV-1a(code) & (snap_slots-1)
  uint64_t s0 = s.seq.load(std::memory_order_relaxed);
  s.seq.store(s0 + 1, std::memory_order_release);   // 奇：进入写（release 防下方载荷写重排到本 store 前）
  s.code = ...; s.datetime = ...; ... ;             // 载荷（普通写）
  s.seq.store(s0 + 2, std::memory_order_release);   // 偶：写完（release 保证上方载荷写先于本 store 对读者可见）
}
// 注：release store + 读者 acquire load 已建 happens-before，无需额外 seq_cst fence（二审 P2-4 清理）

// 读者（分析进程，无锁）
bool get(const char* code, QuotePOD& out) {
  SnapSlot& s = slot_of(code);
  uint64_t s1 = s.seq.load(acquire);
  if (s1 & 1) return false;               // 写中，本次跳过（下轮再读）
  memcpy(&out, &s.payload, ...);          // 注意只拷载荷，不拷 seq
  uint64_t s2 = s.seq.load(acquire);
  return s1 == s2;                        // 不等则被写者穿插，调用方重试
}
```

> 哈希冲突：同槽不同 code 时，读者比对 `s.code`，不匹配则视为未命中（线性探测或接受 miss；快照表 8000 代码 / 8192 槽负载因子≈0.98 偏高，**槽数上调到 16384 = 4MB** 更稳，见 Open Questions）。

### 5.2 时序 ring（单写多读，读者各自 cursor）

seq 编码见 §四：**奇=写中、偶=完成，round = seq/2**。

```cpp
// ---- 写者（采集主线程，唯一写者）----
uint64_t push(const PayloadPOD& p) {
  uint64_t pos = h.head.load(std::memory_order_relaxed);   // 单写者，pos 唯一（无需 fetch_add 占位）
  Slot& sl    = slots[pos % capacity];
  uint64_t round = pos + 1;                              // 第几轮（从 1 起）
  sl.seq.store(2*round + 1, std::memory_order_relaxed);  // 奇：占位，读者见此重试本槽（不丢）
  std::atomic_thread_fence(std::memory_order_release);
  sl.payload = p;
  std::atomic_thread_fence(std::memory_order_release);
  sl.seq.store(2*round + 2, std::memory_order_release);  // 偶：可读
  h.head.store(pos + 1, std::memory_order_release);      // 写完才推进（head 兼任「已写完」语义；二审 P1-new 修复）
  h.total_pushed.store(pos + 1, std::memory_order_relaxed);  // = head（单写者）
  return pos;
}

// ---- 读者（各自 cursor，非递归；评审 P1-2 修复）----
// 返回 >cursor：成功（out 已填）；==cursor：无新数据或写中（稍后重试）；<cursor：被覆盖跳进
uint64_t read_from(uint64_t my_cursor, PayloadPOD& out) {
  uint64_t head = h.head.load(std::memory_order_acquire);
  if (my_cursor >= head) return my_cursor;              // 无新数据
  uint64_t oldest = (head >= capacity) ? (head - capacity) : 0;  // 最旧可用（防御下溢，二审 P2-1）
  if (my_cursor < oldest) my_cursor = oldest;           // 落后超容量 → 跳到最旧可用
  Slot& sl = slots[my_cursor % capacity];
  uint64_t round = my_cursor + 1;
  uint64_t tag  = sl.seq.load(std::memory_order_acquire);
  if (tag == 2*round + 1) return my_cursor;             // 写中（奇）→ 不前进，重试本槽，不丢数据
  if (tag != 2*round + 2) return oldest;                // 偶但 round 不符 → 已被覆盖，跳进
  std::atomic_thread_fence(std::memory_order_acquire);
  out = sl.payload;
  if (sl.seq.load(std::memory_order_acquire) != tag) return oldest;  // 读期间被覆盖
  return my_cursor + 1;                                 // 成功，推进
}
```

调用方用循环 drain（**非递归**）：

```cpp
uint64_t c = my_cursor;
while (true) {
  uint64_t n = ring.ReadFrom(c, out);
  if (n == c) break;               // 无新数据 / 写中，结束本轮
  if (n > c) { consume(out); c = n; }   // 成功
  else { c = n; }                  // n < c：被覆盖跳进，继续循环
}
my_cursor = c;
```

### 5.3 写者存活检测（分析进程）

```
uint64_t hb = header.writer_heartbeat_epoch.load(acquire);
uint64_t gen = header.generation.load(relaxed);
// 若 now - hb > N × interval 且 seq 卡奇 → 写者已死，段陈旧
//   读者可选择：用最后快照（容忍陈旧）/ 标记 stale 等待新写者
```

### 5.4 读者生命周期状态机（评审 P1-3 修复）

读者须处理段不存在、段重建、写者死亡三种场景：

```
   段文件不存在 / magic 校验失败
        ┌────────────── INIT（1s 退避重试 OpenReadOnly）
        │                    │ 段出现 + magic/version 通过
        │                    ▼
        │              ATTACHED（读，每读校验 generation）
        │                    │ generation 变化 / 写者死亡
        │                    ▼
        └──────────────── STALE ──► REATTACH（munmap + 重新 OpenReadOnly + cursor 归零）
                                       │
                                       └─► ATTACHED
```

- **INIT**：段不存在（采集进程未启动）时读者**不报错退出**，按 1s 退避重试；magic/version 校验失败亦重试（写者可能正在重建）。
- **ATTACHED**：正常读；每次读前后校验 `header.generation` 是否变化。
- **STALE → REATTACH**：检测到 (a) `generation` 变化（写者重建段），或 (b) `now − writer_heartbeat > N×interval` 且某槽 `seq` 卡奇（写者死亡）→ **立即 munmap 旧映射**（旧映射指向被重建的 tmpfs 文件，会读到零页/旧内容，不可继续用）→ 重新 `OpenReadOnly` → cursor 归零（ring 已清零）→ 回 ATTACHED。
- **freeze 协议**（段重建期）：写者重建段前先置 `generation = 0xFFFF…F`（重建中标记），读者见此立即进 STALE 暂停读；写者清零 + 重新 ftruncate 完成后写新 generation，读者 REATTACH。

---

## 六、架构与改造点

### 6.1 新增模块 `src/shm/` + `include/tdx/shm/`

```
include/tdx/shm/
  segment.hpp      SegmentHeader / 段映射（创建/挂载/校验 magic）
  snapshot.hpp     SnapSlot / SnapshotWriter / SnapshotReader（seqlock）
  ring.hpp         RingHeader / ring<T> 模板（push / read_from）
  payload.hpp      QuotePOD / TransactionPOD / KLinePOD / HistOrderPOD（与 tdx::types 双向转换）
src/shm/
  segment.cpp      mmap/shm_open/ftruncate/MSYNC 实现
  CMakeLists.txt   add_library(tdx_shm STATIC ...)，仅依赖 tdx::types + 标准库
```

**CMake 集成**：`src/CMakeLists.txt` 加 `add_subdirectory(shm)`；`tdx` CLI target 链接 `tdx_shm`。纯标准库，**无新外部依赖**，无需改 `scripts/setup_external.sh`。

### 6.2 `fetch_quotes.cpp` 改造（阶段B 拆分）

```cpp
// 阶段B 改造后（主线程）
auto shm = tdx::shm::Segment::OpenOrCreate(path, layout);   // 写者
auto ingest_q = std::make_shared<IngestQueue>(capacity);     // SPSC
std::thread ingest_worker(IngestLoop, ingest_q, taos_cfg);   // 入库线程
// ... 每轮 RunOneRound 的聚合后：
for (auto& c : chunks) {
  for (auto& q : c.quotes)  { shm.Snapshot().Put(q.code, to_pod(q)); ingest_q->Push(q); }
  for (auto& t : c.txns)    { shm.Ring<TransactionPOD>().Push(to_pod(t)); ingest_q->Push(t); }
  // ... 同理 tick/kline/hist_ord
}
shm.Header().writer_heartbeat_epoch = now_epoch;             // 心跳
// 入库由 ingest_worker 异步处理，主线程立即进入下一轮 fetch
```

入库线程 `IngestLoop`：drain 队列 → 复用现有 `InsertQuote/InsertTx/...`（`fetch_quotes.cpp:212-473`，**几乎不改**）→ 批量 `taos_query`。队列满覆盖最旧。

### 6.3 分析进程（读者）示例

```cpp
auto shm = tdx::shm::Segment::OpenReadOnly(path);            // 只读挂载
auto& snap = shm.Snapshot();
QuotePOD q;
if (snap.Get("600000", q)) { /* 用 q.price ... */ }
auto& ring = shm.Ring<TransactionPOD>();
uint64_t cursor = my_last_cursor;                            // 进程内持久
while (true) {
  TransactionPOD t;
  cursor = ring.ReadFrom(cursor, t);                         // 无锁读增量
  if (consumed) analyze(t);
}
```

### 6.4 读侧获取模式（所有分析进程遵循的约定）

**核心约定：共享段只承载「原始事实」——快照表存最新态、ring 存全局时序事件流；按 code 的聚合视图由各读进程按需在进程内构建，不进共享段。** 多读进程需求各异，per-code 视图是消费者私有，强塞进共享段会污染单一事实源。

读进程按数据形态走三条路径：

| 数据形态 | 路径 | 成本 |
|---|---|---|
| 单只最新态（现价/五档/量额） | `snap.Get(code)` | **O(1)**，无消费者状态，纳秒级 |
| 全市场扫描（选股） | 遍历快照表槽 | O(slots)，2MB 顺序扫，微秒级 |
| 单只当日事件序列（逐笔/委托/分钟K） | ring 增量 + 进程内订阅集聚合 | O(1) 命中本地容器 |

**当日序列的侧聚合（关键）：** ring 是**全局混排**时序流，读「某只当日全部逐笔」不能 O(1) 从 ring 直接取。读进程须维护订阅集（通常几十只）+ per-code 累积容器，后台线程持续 drain ring 增量按 code 分流：

```cpp
struct ReaderState {
  uint64_t txn_cursor = 0;                                         // 进程内持久
  std::unordered_set<std::string> sub = {"600000","000001", /*...*/};
  std::unordered_map<std::string, std::vector<TransactionPOD>> txns;
};
// 后台 drain：分析进程非 helio 用 std::this_thread::sleep_for；helio 应用用 ThisFiber::SleepFor
while (!stop) {
  TransactionPOD t;
  while (seg.Ring<TransactionPOD>().ReadFrom(st.txn_cursor, t) != st.txn_cursor)
    if (st.sub.count(t.code)) st.txns[t.code].push_back(t);
  std::this_thread::sleep_for(50ms);
}
auto& vec = st.txns["600000"];   // 查询：O(1) 命中本地容器
```

订阅子集聚合内存可控（单只一日数千笔 × 40B）；全市场逐笔历史属回测范畴，应走 TDengine 而非 shm。

**冷启动去重衔接：** ring 容量按峰值估为**分钟级回溯窗口**，读进程非开盘启动时，开盘至启动前的当日全量须从 TDengine 回填，再以 ring 增量接续、按主键去重：

```cpp
uint64_t base = seg.Ring<TransactionPOD>().head();
auto hist = taos_query("SELECT * FROM tdx.trans WHERE code='600000' AND ts>=today");
st.txns["600000"] = parse(hist);
st.txn_cursor = base;            // 之后只消费 ring 增量
// ring 最早几条可能与 hist 末尾重叠 → 按 trans_id 去重
```

三者分工：**TDengine = 当日全量早段，shm ring = 实时增量近段，快照表 = 当前态。**

**布局决策：采纳「全局 ring + 侧聚合」，per-symbol ring 列为 Won't。** 读侧主场景为订阅几十只消费增量流（评审选项 a），全局 ring 内存紧凑（~48MB）、写者最简，足够。per-symbol ring（每 code 独立 ring，读单只当日序列 O(1)）对「频繁查单只当日历史」更友好，但当前读侧模式不需要；若未来实测该场景成瓶颈，再作增强引入（见 §10 Q7）。

---

## 七、分阶段实施（MoSCoW）

| 阶段 | 范围 | Success Signal |
|---|---|---|
| **6.1 MVP** | 快照表 + seqlock + 异步入库骨架（SPSC + IngestWorker 线程）；CLI 加 `--mmap-path`；一个 `reader` 示例进程读最新价 | 单元：seqlock 单写多读正确性、SPSC 无丢失；集成：fork 子进程读到写入价；采集节拍与入库解耦（入库慢不拖 fetch） |
| **6.2 逐笔/委托流** | Transaction / HistoryOrder ring + 读者 cursor 推进 | 单元：ring 覆盖语义、读者慢消费跳过正确；真网：fetch-quotes + reader 读到逐笔增量 |
| **6.3 增量分钟K** | KLine ring（采集端聚合 5m/1m 增量推送） | czSC/Krono 能从 ring 重建当日分钟序列 |
| **6.4 加固（Should）** | 写者存活检测 + 段重建协议、多采集进程互斥（flock + generation）、崩溃恢复（magic 校验失败重建）、metrics（队列深度/覆盖次数/读写延迟） | 异常场景：写者 kill -9 后读者正确判 stale；两采集进程启动仅一个写 |
| **Won't** | Python 绑定、跨进程事件通知（eventfd/futex 推送，目前轮询足够）、变长字段（F10 等不入 shm，仍走 TDengine） | — |

---

## 八、风险与反对意见

### 8.1 helio 纪律复核（P0）

| 操作 | 线程 | 原语 | 合规？ |
|---|---|---|---|
| 写 mmap 快照/ring | 采集**主线程**（非 Proactor 线程） | `std::atomic` / seqlock | ✅ 无阻塞 |
| 投递 SPSC 队列 | 采集主线程 | lock-free SPSC | ✅ |
| 异步 INSERT TDengine | 独立 `std::thread` | `std::mutex`/`condvar` | ✅ 非 Proactor 线程，豁免 |
| 分析进程读 mmap | 各进程主线程 | `std::atomic` 只读 | ✅ |

**结论**：fiber worker 不触 mmap/队列/TDengine，全部阻塞操作在 Proactor 之外，符合 CLAUDE.md 纪律。

### 8.2 反对意见（已评估）

1. **「为何不用 Boost.Interprocess？」** → 见 D1：依赖重 / 违反 fiber 纪律 / 红黑树负优化 / 黑盒簿记。
2. **「为何不让分析进程直接订阅 TDengine TMQ？」** → TMQ 行级订阅有连接 + 反序列化开销，全市场高频轮询延迟远高于本地 mmap O(1)；且分析进程要的是「当前全市场快照视图」，mmap 更贴合。TDengine 保留为持久层/历史回溯。
3. **「MPMC ring 多消费者覆盖风险？」** → 读者各自 cursor，被覆盖时跳到最旧可用（ring 标准语义）；容量按峰值估够分钟级回溯；6.4 加 total_pushed 指标暴露覆盖次数。

### 8.3 一致性风险

- **写 mmap 与入库的顺序**：先写 mmap（对分析进程可见）再投递入库队列。即使入库丢数据，分析进程已看到 → **mmap 是「已观测」的最新真值，TDengine 是「已持久化」的副本**，两者语义分层清晰。
- **seqlock 奇数卡死**：写者写到一半崩溃 → `seq` 永奇。读者检测到（`now - heartbeat > N×interval` 且 seq 奇）即判 stale；新写者启动 `generation+1` 并重置段。

### 8.4 评审修复追踪（v0.2）

评审（`reviews/phase6-intraday-shm.review.md`）的 P0/P1 已在本版修复：

- **P0-1 原子性前提** → §四「布局不变量」`static_assert` + §5.0.1 平台说明 + §九 双进程实测。
- **P0-2 单写者 invariant** → §5.0.2 `writer_pid` 探活 + flock 全周期持有。
- **P1-1 ring 死字段** → §四 RingHeader 删 `committed`，统一 per-slot seq。
- **P1-2 read_from 不闭环** → §5.2 非递归 + 明确 seq 编码（奇偶/round），写中槽重试不丢数据。
- **P1-3 读者生命周期** → §5.4 INIT/ATTACHED/STALE/REATTACH 状态机 + freeze 协议。

**二审（v0.3）追加修复：**

- **P1-new head 语义回归** → §5.2 push 的 `head` 改「写完才推进」（`load`/`store` 替代 `fetch_add`），消除 read_from 误判覆盖丢数据。
- **P2-1** → read_from 用 `oldest = (head>=capacity)? head-capacity : 0` 防御下溢。
- **P2-4** → §5.1 put 删除冗余 `seq_cst` fence（release/acquire 已建 happens-before）。

其余 P2/P3（drain backoff、REATTACH 时 per-code 容器重建、pid 复用、含 atomic 成员结构体的 placement new 初始化）见 §九与 §十，随 MVP 推进落地。

---

## 九、测试策略

| 层 | 用例 | 标记 |
|---|---|---|
| 单元 | seqlock 并发读写正确性（多线程读者 + 单写者，无撕裂）；SPSC 队列无丢失/无重复；ring 覆盖语义 + 写中槽不丢；段头 magic/version 校验；**POD↔types.hpp 全字段转换覆盖**（评审 P2-1）；布局 `static_assert`（lock-free/alignas/offsetof）编译期通过；**flock + writer_pid 探活：第二写者启动被拒** | unit |
| 集成 | `fork()` 子进程模拟分析进程，父写子读快照/ring 一致；**双进程原子性长时压（父写子读无撕裂，验证 P0-1）**；**读者 re-attach：段重建（generation 变化）后正确 munmap+重新挂载+cursor 归零（P1-3）**；**写者 kill -9 后读者判 stale 并降级** | local |
| 真网 | `fetch-quotes --loop --mmap-path ...` + 独立 `reader` 进程，读到实时写入的最新价/逐笔；采集节拍不被入库阻塞（对比改造前后 fetch 间隔） | live |

遵循全局规范：覆盖率 >80%、真实优先于 mock、第三方（系统 mmap）不计覆盖。

---

## 十、Open Questions

> 评审 P0/P1 已在 v0.2 修复（见 §四「布局不变量」、§5.0、§5.2、§5.4）。下表为 P2 级及其余未决项。

| # | 问题 | 倾向 | 状态 |
|---|---|---|---|
| 1 | 快照槽数 8192（负载因子≈0.98）是否需上调？哈希冲突策略？ | **槽数定为 16384（4MB）；冲突策略定为开放寻址 + 线性探测**（评审 P2-3） | ✅ 已定 |
| 2 | 是否需要 Tick（分时）ring？czSC/Krono 是否消费分时？ | 若分析侧不用分时则 Won't | 待用户确认 |
| 3 | 入库队列容量与覆盖策略（覆盖最旧 vs 丢最新）？ | 覆盖最旧（最新优先） | 待评审定 |
| 4 | 多采集进程场景是否真实存在（需互斥）？ | 单采集进程为主，6.4 预留 flock | 待用户确认 |
| 5 | 是否需要跨进程事件通知（eventfd）做毫秒级推送？ | 轮询足够（协议上限 30s），Won't | 暂不 |
| 6 | 段布局 version 升级时的兼容策略（读者发现 version 不符）？ | 版本不符即拒绝挂载并报错 | 待评审定 |
| 7 | 是否需要 per-symbol ring（每 code 独立 ring，读单只当日序列 O(1)）？ | 当前 **Won't**（全局 ring + 侧聚合足够）；触发条件：实测「频繁查单只当日历史」成瓶颈 | 触发式 |
