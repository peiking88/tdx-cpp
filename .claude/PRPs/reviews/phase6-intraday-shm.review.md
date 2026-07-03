# Phase 6 共享内存设计 — 架构评审报告

*评审对象*：`/home/li/peiking88/tdx-cpp/.claude/PRPs/prds/phase6-intraday-shm-design.md`（v0.13.0 DRAFT）
*评审日期*：2026-07-03
*评审方法*：architecture-review 技能（MMI / anti-patterns / checklist）+ 对抗性技术审查（seqlock / 跨进程原子语义 / helio 纪律）
*评审者*：Claude

---

## 一、评审结论

### 最终判定：⚠️ REQUEST CHANGES

**方向正确，不需要推倒重来**：裸 mmap + 定长 POD + seqlock + ring 是「单写者采集、多读者低延迟」跨进程行情共享的**正解**；D1 排除 Boost.Interprocess（fiber 纪律 + 黑盒簿记）论证扎实；数据结构定长性逐字段核实（`types.hpp`）；§6.4「共享段只存原始事实、聚合由消费者做」是好设计；分阶段 MoSCoW 合理。

**但存在 2 个 P0 与 3 个 P1**，必须在进入 Phase 6.1 MVP 前修复——它们都落在「方案赖以成立的基石」上：

| 级别 | 问题 | 一句话 |
|---|---|---|
| **P0-1** | `std::atomic` 跨进程 lock-free 假设未验证/未强制 | 全文用 `std::atomic<uint64_t>` 但无 `is_always_lock_free` 断言；标准不保证跨进程语义 |
| **P0-2** | seqlock 单写者 invariant 无运行期保障 | 多采集进程或误入 fiber 即静默数据损坏，flock 仅启动期选主不够 |
| **P1-1** | ring 协议内部不一致 | §4 定义 `committed` 字段，§5.2 协议完全没用——死字段或协议漏写 |
| **P1-2** | ring `read_from` 协议不闭环 | 递归调用、`WRITTEN_BIT` 未定义、写中槽被误判覆盖而丢数据 |
| **P1-3** | 读者生命周期协议缺失 | 段不存在 / 段重建（generation 变化）/ 写者死亡时读者行为未定义 |

修复 P0/P1 后**只需重读 delta**，不需全文重审。

---

## 二、MMI 评分（Modular Maturity Index）

| 维度 | 评分 | 说明 |
|---|---|---|
| **Modularity（模块独立性）** | 8/10 | `tdx_shm` 模块独立、依赖清晰（`tdx::types` + 标准库）。扣分：`fetch_quotes.cpp:212-473` 的 `InsertQuote/...` 与 shm 写 + 入库队列耦合在同一改造点（§6.2），MVP 后应抽 IngestQueue/shm-write 到独立翻译单元 |
| **Layering（分层组织）** | 9/10 | 采集进程 → shm → 分析进程，单向无倒置；快照表 / ring 分区按访问模式正交划分 |
| **Pattern Consistency（模式一致性）** | 6/10 | 扣分集中：ring 的 `head/committed` 定义与 §5.2 协议不一致（P1-1）；seqlock/ring 伪码有未闭环处（P1-2）；`std::atomic` 用法缺 lock-free 断言（P0-1） |

**综合 MMI：7.7/10** —— 结构健康、决策扎实，但并发协议描述的严谨性是短板（这正是 shm 方案风险最集中的地方）。

---

## 三、Anti-Pattern 扫描

### ✅ 未检出（健康）
- **Big Ball of Mud** —— 分层与分区清晰
- **Golden Hammer（#9）** —— mmap 是跨进程低延迟的恰当工具，非强行套用；且对 Boost/TMQ/per-symbol ring 都做了取舍
- **Vendor King（#18）** —— 裸 mmap 零厂商依赖，纯标准库
- **Architecture by Implication（#17）** —— 决策日志（D1–D6）完整、有反证

### ⚠️ 需警惕（已定位）

| Anti-Pattern | 位置 | 严重度 | 处置 |
|---|---|---|---|
| **Premature Optimization（#10）** | ring 100万槽×3 + SPSC + 异步入库线程，全部在「未量化延迟需求」下设计；协议上限本就是 30s poll | **P2-4** | MVP（6.1）只做快照表+异步入库，把「ring 是否必要」作为 MVP 后 gate（fitness function：量采集节拍改善与读者延迟） |
| **Dual Schema（#12）** | `QuotePOD/TransactionPOD/...`（payload.hpp）与 `tdx::Quote/Transaction`（types.hpp）双重表示 + 双向转换 | **P2-1** | 转换层是 bug 温床（code 截断/NaN 哨兵/单位），须全字段单测覆盖；考虑宏/codegen 同步字段 |
| **Database as IPC（#11）** 倾向 | 现状分析进程只能查 TDengine——本设计正是为消除该反模式而建 | ✅ | 设计目标即解此，方向正确 |

---

## 四、Checklist 维度评估

| 维度 | 状态 | 说明 |
|---|---|---|
| 结构质量（Modularity/Layering） | ✅ 优 | 见 MMI |
| 复杂度 | ⚠️ | seqlock+ring+SPSC+异步入库，复杂度不低；Premature Optimization 风险（P2-4） |
| **可靠性/弹性** | ❌ **缺口** | 单写者 invariant 无保障（P0-2）；读者生命周期/崩溃恢复 freeze 缺失（P1-3、P2-6） |
| **并发正确性** | ❌ **缺口** | 跨进程原子语义未验证（P0-1）；ring 协议不一致/不闭环（P1-1、P1-2） |
| **可测试性** | ⚠️ | seqlock/SPSC 单测列出了；但缺 POD 转换全覆盖、跨进程原子性实测、fitness function（P2-1、P2-7） |
| 性能 | ⚠️ | O(1) 快照定位正确；但 cache line 策略未提（P2）、容量未经实测（P2-4） |
| 安全 | ✅ N/A | 本地 shm，无远端面；注意 `/dev/shm` 文件权限（建议 0600） |
| 可观测性 | ❌ **缺口** | metrics 推到 6.4（Should），MVP 无任何指标验证「节拍是否真解耦」（P2-7） |
| helio 纪律 | ✅ 优 | §8.1 复核表准确：写 mmap 在主线程、入库独立 `std::thread`，fiber worker 不触 shm/队列/TDengine |

---

## 五、P0 问题（阻塞，进 MVP 前必修）

### P0-1：`std::atomic` 跨进程 lock-free 假设未验证、未强制

**位置**：D5（§三）、§四 SegmentHeader/SnapSlot/RingHeader、§五全部伪码。

**问题**：全文对 `std::atomic<uint64_t>`（`seq` / `head` / `committed` / `total_pushed` / `writer_heartbeat_epoch` / `generation`）的跨进程使用建立在一个**未验证、标准也不显式保证**的前提上：
- C++ 标准保证 `std::atomic` 在**单进程多线程**间的语义；**跨进程（不同虚拟地址映射同一物理页）的语义标准未定义**，实践中依赖「lock-free 原子用 CPU 总线级指令（LOCK CMPXCHG / LDAR-STLR），与进程无关」这一**实现事实**。
- 一旦某平台/某字段 `!is_always_lock_free`，`std::atomic` 会退化为**进程内锁**（如 `__gthread_mutex`），在共享内存里跨进程完全失效 → 数据竞争 → UB，且**静默不崩溃**。
- 文档**没有任何 `static_assert(std::atomic<uint64_t>::is_always_lock_free)`**，也没有 `alignas` + `offsetof` 校验关键字段对齐。

**修复**：
1. 所有共享内存中的原子字段，加 `static_assert(std::atomic<uint64_t>::is_always_lock_free)`（编译期拦截非 lock-free 平台）。
2. 关键结构体 `alignas(64)` + `static_assert(offsetof(SnapSlot, seq)==0)` 等校验对齐。
3. 显式注明「依赖平台 lock-free 原子在 MAP_SHARED 上的跨进程行为，标准不保证；MVP 须在目标平台（x86-64/ARM64）实测双进程读写正确性」。
4. 备选：改用 GCC `__atomic_load_n/__atomic_store_n/__atomic_thread_fence` builtins，语义更明确（明确无锁、明确 memory order），且避开 `std::atomic` 可能的成员函数/非平凡布局。

### P0-2：seqlock 单写者 invariant 无运行期保障

**位置**：D3（§三）、§五 5.1/5.2、§十 Q4。

**问题**：seqlock 是**单写者**算法——两个写者各自 `seq+1→写载荷→seq+1` 会互相穿插，`seq` 奇偶语义崩塌，读者读到撕裂数据，**静默损坏**。文档把「写者=采集主线程」作为假设，但：
- §十 Q4 与 §七 6.4 提到「多采集进程」+ flock——但 **flock 只在启动选主时有效**，无法防止运行期两个写者并发写（路径错配、flock 漏持、手抖启动第二实例都会破）。
- **单进程内**若未来有人把 mmap 写误放进 fiber worker（违反 D3），单写者假设即破，且**无任何编译期/运行期拦截**。
- 文档没有把「单写者」升级为**强 invariant + 违反后果（数据损坏，非崩溃）+ 运行期检测**。

**修复**：
1. 段头 `writer_pid` 由写者启动时写入；新写者启动前 `kill(writer_pid, 0)` 探活，若仍存活 → **abort 并报错**（而非 silently 接管）。
2. 写者持有 `flock(LOCK_EX | LOCK_NB)` 全生命周期，拿不到即退出——把「唯一写者」从约定变成强制。
3. 文档显式声明：**mmap 写 API（`Snapshot::Put` / `Ring::Push`）只能由单一进程的单一线程调用**，违反导致静默数据损坏；考虑 CI clang-tidy/命名约定限制调用点。
4. 多采集进程若真需要，须改用**多写者协议**（如 per-writer seq 或 pthread_mutex + `PTHREAD_PROCESS_SHARED`），不能复用单写者 seqlock。

---

## 六、P1 问题（严重，MVP 前应定）

### P1-1：ring 协议内部不一致 —— `committed` 字段定义了却没用

**位置**：§四 RingHeader（`committed`）vs §五 5.2（用 `head` + per-slot `seq` + `WRITTEN_BIT`）。

**问题**：RingHeader 定义了 `std::atomic<uint64_t> committed`（「已提交的位置」），但 §5.2 的 `push`/`read_from` 完全没有读写 `committed`——可读性靠 per-slot `seq` 判断。`committed` 是**死字段**，或协议漏写了批量提交语义。实现者会困惑：到底判 `committed` 还是 per-slot `seq`？

**修复**：二选一并统一文档——
- 方案 A（建议）：删 `committed`，可读性统一靠 per-slot `seq`（更精细，head 已单调）。`total_pushed` 保留做覆盖诊断。
- 方案 B：保留 `committed`，改 §5.2 用 `committed` 判可读（批量提交语义，写者攒批后一次 `committed.store`），per-slot seq 仅判覆盖。需重写伪码。

### P1-2：ring `read_from` 协议不闭环 —— 递归 + WRITTEN_BIT 未定义 + 写中槽误判

**位置**：§五 5.2 读者伪码。

**问题**（三处）：
1. **`WRITTEN_BIT` 未定义**：伪码用 `pos + 1 + WRITTEN_BIT` 标记可读，但 `WRITTEN_BIT` 是哪一位、与「奇=写中」语义如何兼容，未定义。读者无法据此正确判断。
2. **写中槽被误判为「被覆盖」而丢数据**：写者 `push` 先 `seq.store(pos+1)`（占位），写完 `seq.store(pos+1+WRITTEN_BIT)`。读者若在占位与写完之间读到 `tag=pos+1`，则 `tag != pos+1+WRITTEN_BIT` → 判「被覆盖」`my_cursor++` 跳过——**丢失一条本可读到（稍后重试即可）的数据**。语义错。
3. **递归 `read_from(...)`**：连续多槽被覆盖时栈递归加深，且返回值语义混乱（返回新 cursor 但 `out` 未填，调用方难以区分「无新数据」「被覆盖跳过」「成功读取」）。

**修复**：重写 ring 读者为**非递归、明确状态码**的循环；seq 编码清晰定义为 `seq = 2*round + {0=完成, 1=写中}`（奇偶），读者见奇则**重试本槽**（不前进），见偶且 round 匹配则读载荷、cursor+1，见偶且 round 不匹配则被覆盖、跳到 `head-capacity`。给非递归伪码。

### P1-3：读者生命周期协议缺失 —— 段不存在 / 段重建 / 写者死亡

**位置**：§五 5.3（仅给判 stale 的片段）、§六 6.3/6.4（假设段已存在）。

**问题**：文档没回答读者侧三个关键场景：
1. **段文件不存在**（采集进程未启动）时，`OpenReadOnly` 失败 → 读者报错退出？阻塞等待？重试？未定义。
2. **写者重建段**（generation+1，ftruncate 清零 76MB）：读者持有的旧 mmap 映射指向被重建的 tmpfs 文件，**行为未定义或读到旧内容/零页**；读者须感知 generation 变化并**重新 mmap**，但 §5.3 只说「读者据此判断」，没说判断后如何 re-attach。读者 cursor 也须随段清零而重置。
3. **写者死亡**（heartbeat 停）：读者判 stale 后是继续用最后快照（容忍陈旧）还是暂停？未定义状态机。

**修复**：补一节「读者生命周期状态机」：
```
INIT(等段出现, 退避重试) → ATTACHED(读, 监听 generation)
  → STALE(gen 变化 / 写者死 / seq 卡奇) → REATTACH(munmap+重新 OpenReadOnly, cursor 归零) → ATTACHED
```
明确段重建期间的 freeze 协议（写者重建时置 generation 为「重建中」标记，读者见此暂停读）。

---

## 七、P2 / P3 问题（不阻塞 MVP，记录跟踪）

| # | 问题 | 位置 | 建议 |
|---|---|---|---|
| P2-1 | Dual Schema：POD ↔ types.hpp 双向转换 | payload.hpp | 全字段转换单测；考虑宏同步字段 |
| P2-2 | 冷启动去重键不明确（trans_id 跨日可能不唯一；datetime 秒 vs TDengine ts 毫秒精度不一致） | §6.4 | 明确去重键 = (交易日, trans_id)，精度对齐 |
| P2-3 | 快照表哈希冲突策略「线性探测或接受 miss」未定；负载因子 0.98 偏高 | §5.1 | 定策略（建议开放寻址+线性探测）+ 槽数 16384（§10 Q1） |
| P2-4 | Premature Optimization 风险——延迟需求未量化 | 全局 | MVP 只做快照表+异步入库，ring 作为 MVP 后 gate |
| P2-5 | memfd_create 未评估 | D2 | 补一句：memfd 跨进程需 fd 传递（SCM_RIGHTS/proc），复杂度 > 路径约定，故选 /dev/shm |
| P2-6 | 崩溃恢复「重置段」期间读者会看到不一致数据 | §8.3 | 加 freeze 协议（重建中标记），读者暂停读 |
| P2-7 | MVP 无可观测性 | §7 6.4 | MVP 至少带：写者每轮耗时、入库队列深度、seqlock 读者重试次数——验证「节拍是否真解耦」 |
| P2 | SnapSlot 的 seq 与 payload 同 cache line，写者写污染读者 seq 缓存 | §4 SnapSlot | 写频率 30s，影响小；若采集提速则 seq 独占一 cache line |
| P3 | `code[8]` 容纳 7 字符，扩展市场代码可能截断 | payload.hpp | intraday 仅 A股(6位) OK，加 assert + 单测 |
| P3 | live 测试非盘中时段会 SKIP | §9 | 补离线回放测试（录制的 chunk → shm） |

---

## 八、值得肯定的设计（保留）

1. **D1 排除 Boost.Interprocess** —— 抓住 `interprocess_mutex` 违反 helio fiber 纪律这一关键点，论证扎实；裸 mmap 零新依赖、定长 O(1) 优于红黑树。
2. **数据结构定长性逐字段核实** —— `types.hpp` 算到字节，`Quote` POD 化（`name` 恒空、`code` 6字节）有 `parsers_quotes.cpp:90` 证据支撑，布局有据。
3. **§6.4 读侧约定** —— 「共享段只承载原始事实，per-code 聚合由消费者按订阅集在进程内做」是正确的分层，避免把消费者私有视图污染进单一事实源。
4. **§8.1 helio 纪律复核表** —— 写 mmap 在主线程、入库独立 `std::thread`、fiber worker 不触阻塞操作，全合规。
5. **反对意见评估**（Boost / TMQ / per-symbol ring）—— 取舍清晰，不回避。
6. **分阶段 MoSCoW** —— MVP 聚焦快照表+异步入库，合理。

---

## 九、修复后重审范围 + Action Plan

修复 P0/P1 后，**只需重读 delta**（P0-1/P0-2 的断言与保障机制、P1-1 的 ring 协议统一、P1-2 的非递归 read_from、P1-3 的读者状态机），不需全文重审。P2/P3 可在实现阶段顺带处理或记 issue。

### Action Plan

| 问题 | 优先级 | 修复动作 | 验收 |
|---|---|---|---|
| P0-1 atomic 跨进程 | P0 | 加 `is_always_lock_free` static_assert + alignas/offsetof；注明平台依赖；评估改 `__atomic_*` | 编译期断言通过；MVP 含双进程原子性实测 |
| P0-2 单写者 invariant | P0 | 段头 writer_pid 探活 + flock 全周期持有 + 文档声明违反后果 | 第二写者启动被拒；调用点受限 |
| P1-1 ring committed | P1 | 删 committed 或改协议，文档统一 | 无死字段，协议自洽 |
| P1-2 read_from | P1 | 非递归 + 明确 seq 编码 + 写中槽重试不丢 | 单测：写中并发读不丢数据 |
| P1-3 读者生命周期 | P1 | 补 INIT/ATTACHED/STALE/REATTACH 状态机 + freeze 协议 | 段重建后读者正确 re-attach |
| P2-4 premature opt | P2 | MVP 只做快照表+异步入库，ring 设为 gate | fitness function 量化节拍改善 |

---

**评审者**：Claude（architecture-review + 对抗性技术审查）
**日期**：2026-07-03
**判定**：REQUEST CHANGES —— 修 P0/P1 后通过

---

# 二审（v0.2 delta，2026-07-03）

*评审对象*：`phase6-intraday-shm-design.md` v0.2（一审 P0/P1 修复后）
*评审重点*：对抗性核查「修订是否真闭环、是否引入回归」——尤其是 P1-1「删 `committed`」+ §5.2 写者 `head` 语义的自洽性。

## 二审判定：⚠️ 仍 REQUEST CHANGES（修订引入 1 个新 P1）

一审 P0/P1 主体关闭，但 P1-1「删 committed」与 §5.2 写者 `head.fetch_add` 预留写法叠加，引入**一个新 P1**（read_from 误判覆盖 → 丢数据）。修复仅一行（head 改「写完推进」），修后 P0/P1 全清。

## 逐项核对

| 一审项 | v0.2 状态 | 说明 |
|---|---|---|
| P0-1 atomic lock-free | ✅ **关闭** | §四 `static_assert(is_always_lock_free)` + §5.0.1 平台说明 + §九双进程实测要求 |
| P0-2 单写者 invariant | ✅ **关闭** | §5.0.2 `writer_pid` 探活 + flock 全周期持有 |
| P1-1 ring committed 死字段 | ⚠️ 删对了，但见 P1-new | committed 删除本身正确，但 head 语义需同步调整 |
| P1-2 read_from 不闭环 | ⚠️ 主体修复，但见 P1-new | 非递归 + seq 编码 + 写中槽不丢，均达成；head 语义残留 |
| P1-3 读者生命周期 | ✅ **关闭** | §5.4 INIT/ATTACHED/STALE/REATTACH 状态机 + freeze 协议 |

## 🔴 P1-new（二审新发现，修订回归）：head fetch_add 预留 → read_from 误判覆盖丢数据

**位置**：§5.2 `push`（行 306 `h.head.fetch_add(1, std::memory_order_relaxed)`）联动 `read_from`。

**机制**：写者 `head.fetch_add` 在**写槽之前**推进 head。读者看到 `head=N` 时，槽 `N-1` 可能尚未开始写——其 `seq` 仍是上一轮回绕残留的偶值 `2*(N-1-capacity)+2`。读者期望 `round=N`，`tag = 2*(N-1-capacity)+2 ≠ 2*N+2` → 走「覆盖」分支，cursor 跳进 `head-capacity`，**永久跳过槽 N-1**（即使稍后写完）→ **丢数据**。

**根因**：删 `committed` 后，head 必须兼任「已写完可读位置」语义，但 `fetch_add` 是「预留位」语义——两者冲突。seq 编码无法区分「新槽未写」（seq=旧轮偶）与「旧槽被覆盖」（seq=新轮偶），二者都是「非期望 round 的偶」。

**修复（一行，保留「无 committed」成果）**：head 改为「写完才推进」，单写者用 load/store 替代 fetch_add：

```cpp
uint64_t push(const PayloadPOD& p) {
  uint64_t pos = h.head.load(std::memory_order_relaxed);   // 单写者，无需 fetch_add 占位
  Slot& sl = slots[pos % capacity];
  uint64_t round = pos + 1;
  sl.seq.store(2*round + 1, std::memory_order_relaxed);    // 奇占位
  std::atomic_thread_fence(std::memory_order_release);
  sl.payload = p;
  std::atomic_thread_fence(std::memory_order_release);
  sl.seq.store(2*round + 2, std::memory_order_release);    // 偶可读
  h.head.store(pos + 1, std::memory_order_release);        // 写完才推进（替代 fetch_add）
  h.total_pushed.fetch_add(1, std::memory_order_relaxed);
  return pos;
}
```

读者 `head.load(acquire)` 看到的位置之前所有槽均已写完，「未写」状态消失，覆盖判定回归正确。

## P2/P3 残留（不阻塞，随实现处理）

| # | 问题 | 建议 |
|---|---|---|
| P2-1 | read_from 的 `head-capacity` 依赖「覆盖必先回绕」隐含不变量，缺防御性下溢保护 | 改 `(head >= capacity) ? head-capacity : 0` |
| P2-2 | read_from drain 循环在读者持续慢于写者时会空转跳进（活锁边缘） | 循环加 backoff（sleep/yield）或限制单轮跳进次数 |
| P2-3 | §5.4 REATTACH 只说 cursor 归零，未提读者 per-code 聚合容器（§6.4 `txns`）清空 + 重新冷启动衔接 | REATTACH 后清空 per-code 容器并重走 §6.4 TDengine 回填 |
| P2-4 | §5.1 快照写者的 `atomic_thread_fence(seq_cst)` 冗余 | release/acquire 配对已建 happens-before，seq_cst fence 多余（性能微损），可删 |
| P3-1 | writer_pid 探活 `kill(pid,0)` 的 pid 复用风险 | flock 是主导保障（进程死亡内核释放锁），pid 探活为辅，可接受 |
| P3-2 | SegmentHeader / SnapSlot 含 `std::atomic` 成员 → 不可拷贝，初始化须 placement new / 逐字段赋值 | 实现注意，文档补一句 |
| P3-3 | `static_assert` 归属头文件未定 | 放 `segment.hpp`，静态库编译即校验 |
| P3-4 | §九 fork 测试验证原子性 OK，但 re-attach 生命周期需独立 exec 进程测 | 补一个独立 reader 进程的 re-attach 集成用例 |

## MMI 复评

| 维度 | 一审 | 二审 | 说明 |
|---|---|---|---|
| Modularity | 8 | 8 | 不变 |
| Layering | 9 | 9 | 不变 |
| Pattern Consistency | 6 | 7.5↑ | committed/WRITTEN_BIT 清除、seq 编码明确、状态机补全；扣分：head 语义 P1-new |
| **综合** | 7.7 | **7.8** | 修 P1-new 后应到 8.0+ |

## 结论

P0 全清、P1 主体修复，但**修订自身引入一个 P1-new**（head 语义），必须修（一行：`fetch_add` → 写完 `store`）。修后 P0/P1 全部关闭，可进 Phase 6.1 MVP。P2/P3 随实现落地。

**二审判定**：REQUEST CHANGES —— 修 P1-new（head 写完推进）后通过，进 MVP。

---

**二审者**：Claude（architecture-review 对抗性复核）
**二审日期**：2026-07-03
