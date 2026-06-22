# tdx-cpp 架构评审报告

*评审对象*：`/home/li/peiking88/tdx-cpp/.claude/PRPs/prds/tdx-cpp.prd.md`
*评审日期*：2026-06-22
*评审方法*：architecture-review 技能（MMI/DORA/checklist/anti-patterns）+ 独立 staff-reviewer 对抗性评审
*评审者*：Claude（综合 staff-reviewer agent 发现 + checklist/anti-pattern 框架独立分析）

---

## 一、评审结论

### 最终判定：⚠️ REQUEST CHANGES

**核心方向正确，不需要推倒重来**：三层合并（协议/行情接口/数据管理）、helio 选型、功能对等优先、三阶段推进——这些战略判断都站得住。helio 选型有代码级证据支撑（`examples/echo_server.cc`、具体 link target），不是凭感觉。

**但存在 2 个结构性 P0 问题**，会直接影响 Phase 1 能否按 Success Signal 验收，必须在进入实现前修复：
1. **Phase 划分与 MoSCoW 自相矛盾**：RetryPolicy/CircuitBreaker 被列为 Must 却放到 Phase 3，导致 Phase 1/2 在没有弹性层保护下连真服。
2. **helio fiber 纪律的传导成本被严重低估**：被当作"CI 扫一下"，实际是全库级隐形陷阱。

修复后只需重读 delta，不需要全文重审。

---

## 二、MMI 评分（Modular Maturity Index）

| 维度 | 评分 | 说明 |
|---|---|---|
| **Modularity（模块独立性）** | 8/10 | 三层清晰，依赖收敛在 proto 层（data 层不直接碰 FiberSocket）。扣分：`SourceRegistry` 虚函数插件过早抽象（P2-2） |
| **Layering（分层组织）** | 9/10 | 依赖方向正确：CLI → data → quotes → proto → helio，无倒置、无循环 |
| **Pattern Consistency（模式一致性）** | 7/10 | 工厂分发、统一 API 一致；扣分：`KLine.datetime` Dual Schema（新增发现）、`TdxData` God Object 倾向（新增发现） |

**综合 MMI：8.0/10** —— 结构健康，但有可清理的抽象瑕疵和两处语义不一致。

---

## 三、Anti-Pattern 扫描

### ✅ 未检出（健康）
- Big Ball of Mud（无架构）—— PRD 分层明确
- Spaghetti Code —— 协议层有 parser 注册表，控制流清晰
- Premature Optimization —— v1 明确不做性能优化
- Architecture by Implication —— PRD 详尽，Decisions Log 完整
- Vendor King —— helio/dragonfly 均开源

### ⚠️ 需警惕（已定位）
| Anti-Pattern | 位置 | 严重度 | 处置 |
|---|---|---|---|
| **Golden Hammer**（#9） | helio fiber 纪律污染全库并发写法 | P0 | 见 P0-2 |
| **Dual Schema**（#12） | `KLine.datetime` "epoch 或 YYYYMMDD" 两种语义混在一个 int64 | P2 | 新增发现，见 P2-4 |
| **God Object**（#8）倾向 | `TdxData` 聚合 10+ fetch_* 方法 | P2 | 继承自上游，v1 保持对等，标注风险 |
| **Ravioli/Lasagna**倾向 | `SourceRegistry` 无第二实现却引入抽象 | P2 | 见 P2-2 |

---

## 四、Checklist 维度评估

| 维度 | 状态 | 说明 |
|---|---|---|
| 结构质量（Modularity/Layering） | ✅ 优 | 见 MMI |
| 复杂度 | ✅ 可控 | 结构体 + 注册表，无过度嵌套 |
| **可靠性/弹性** | ❌ **缺口** | 熔断器错位（P0-1）；缺优雅关闭（P2-1） |
| **可测试性** | ⚠️ **缺口** | 黄金字节流单测好；但缺 helio fiber 异步时序测试（P1-3） |
| **部署** | ⚠️ **metric 张力** | helio 拉 Boost+abseil+glog+liburing，与"部署轻量"目标有张力（新增 P2-5） |
| 性能 | ✅ N/A | v1 不优化，正确 |
| 安全 | ✅ N/A | 通达信协议明文 TCP 是协议特性，非缺陷 |
| 可观测性 | ❌ **缺口** | 无 metrics/结构化日志（P2-1） |
| 数据架构 | ⚠️ | datetime Dual Schema（P2-4）、null 语义未定义（P2-3） |

---

## 五、问题清单（按优先级）

### 🔴 P0 — 必须修复（blocking）

#### P0-1 · Phase 划分与 MoSCoW 自相矛盾，弹性层错位
- **问题**：PRD 把 `RetryPolicy` + `CircuitBreaker`（C11/C12）列为 **Must**（数据管理层），却放到 **Phase 3**。但熔断器本质是**协议层稳定性的必要组件**，不是数据管理能力。结果 Phase 1/2 的 live 测试和"自动重连"在没有熔断保护下连真服——通达信封 IP 是已知中风险，首次连上就触发限流会卡死 Phase 1 验收。
- **佐证矛盾**：Phase 1 宣称"自动重连/自动重试"是 Must，却只靠 helio 心跳 + 协议层 4 次退避，无熔断器，网络抖动下会无限重连打服务器。
- **修复**：把 `RetryPolicy` + `CircuitBreaker` 从 Phase 3 **前移到 Phase 1**（与 Connection/Heartbeat 同批）。Phase 3 只保留 SyncState/GapDetector/HybridSource/Adjust/Resampler/Calendar 纯数据逻辑。同步修正 MoSCoW 归类——熔断器属协议层。
- **来源**：staff-reviewer P0

#### P0-2 · helio fiber 纪律传导成本被严重低估
- **问题**：helio 禁用 `std::mutex`/`std::condition_variable`/`std::thread::sleep_for`，须用 `util::fb2::*`。PRD 只写"CI clang-tidy 扫描"，未评估传导成本：
  1. **SyncState**（Phase 3，JSON 持久化）若用普通 `std::mutex` 保护 JSON 写入，会卡死 Proactor 线程——而它是 v1 必要组件。
  2. **第三方库审计缺失**：nlohmann_json/spdlog/fmt 等若在 fiber 内被调，其内部阻塞原语会出问题。PRD 无"哪些库可安全在 fiber 内调用"清单。
  3. **测试代码是否豁免**：gtest 常用 `std::this_thread::sleep_for`，未说明是否约束测试代码。
  4. **拦截手段不足**：仅 CI lint 不够，应在**头文件层用宏/static_assert 拦截**（编译期）。
- **修复**：
  - Phase 1 Scope 增加"helio 线程安全审计"交付物，逐个确认 fiber 内调用的第三方库无阻塞原语。
  - 把"头文件宏/static_assert 拦截 std::mutex/sleep_for"列为 Phase 1 必做项（编译期，非仅 lint）。
  - 明确测试代码是否豁免 fiber 纪律并写入测试策略。
- **来源**：staff-reviewer P0

### 🟡 P1 — 应修复（non-blocking 但影响质量）

#### P1-1 · "Phase 2 与 Phase 3 部分并行"依据不足
- **问题**：并行声明的前提（Phase 3 非扩展部分只依赖 Phase 1）在 P0-1 修复后（Retry/CircuitBreaker 前移到 Phase 1）部分失效；且 Phase 1 Success Signal 未列出黄金字节流覆盖的 msg_id 清单。
- **修复**：降级为"Phase 1 验收后再评估并行"；Phase 1 Success Signal 显式列出黄金字节流必覆盖 msg_id（至少 0x523 K线/0x547 报价/0x0f 除权/0xfc5 逐笔）。

#### P1-2 · dragonfly 选型应在评审阶段 push back，而非"待确认"延后
- **问题**：dragonfly 是 Redis 兼容 KV，**语义上不适合做 K线主存储**——KV 模型不适合按 [start,end] 范围查询，即使用 Sorted Set 也要全量加载。这是**选型错误，非配置问题**（配 RDB/AOF 也救不了语义错位）。PRD 现在的"待确认"立场过于温和。
- **修复**：本次评审给出明确建议——v2 冷存储用 Parquet/Arrow（正确），dragonfly 仅做热缓存（最新报价/订阅状态，<1MB 量级）。明确告知用户"dragonfly 不适合做 K线主存储"。把 Open Question 从"待确认"改为"已建议分层，等用户确认或反对"。

#### P1-3 · 测试策略遗漏 helio fiber 异步时序测试
- **问题**：测试覆盖了同步逻辑（帧编解码/复权/重采样/熔断状态机），但完全没提：
  - fiber 内异常如何在 fiber 边界传播（未捕获会崩 ProactorPool 还是单 fiber？）
  - 心跳超时（20 次无业务）触发断连的时序测试——`AddPeriodic` 如何在测试里快进时间？
  - 选服并发（`MakeFiber` N 个 fiber）多 fiber 同时写 config.json 的竞态
- **修复**：增加"helio fiber 异步测试"用例类：(a) fiber 异常传播；(b) 心跳/选服时序（明确时间快进手段——mock clock 或 helio TestProactor）；(c) config.json 并发写串行化测试。

### 🟢 P2 — 建议改进（不阻塞）

#### P2-1 · 缺可观测性 / 优雅关闭
- 对嵌入回测/实盘引擎的库，需最小可观测性：结构化日志（连接/熔断/心跳超时事件）+ 优雅关闭接口（drain in-flight fibers）。Phase 1 Scope 增加。指标可推迟。

#### P2-2 · `SourceRegistry`（虚函数插件）在 C++ 里是过度抽象
- tdxdata 的插件注册表在 Python 是装饰器+动态注册；C++ 改虚函数后无运行时插件需求。**保留抽象但没有第二实现 = 为只用一次的代码创建抽象**（违反 CLAUDE.md「简单优先」）。v1 直接用 HybridSource + 几个具体类，未来真要扩展再补抽象。

#### P2-3 · 结构体 null 语义未定义（新增发现）
- `Quote.bid[5]`——盘前只有 1 档买卖盘时，`bid[1..4]` 填什么？0 还是 NaN？0 会被下游回测当成有效报价。需在结构体定义明确"未填字段"语义（`std::optional<double>` 或 sentinel + 文档）。

#### P2-4 · `KLine.datetime` Dual Schema（新增发现）
- 定义为 `int64_t datetime; // epoch 或 YYYYMMDD`——两种语义混在一个字段靠注释区分，是 Dual Schema 反模式。应统一为一种（建议统一为 int64 epoch seconds，或统一为 YYYYMMDD int，分钟线另用字段）。

#### P2-5 · "部署轻量" Success Metric 张力（新增发现）
- Metric 写"单一静态库 + 头文件，无 Python 依赖"，但 helio 拉 Boost(context+system)+abseil+glog+liburing。**"无 Python 依赖"成立，但"轻量"有张力**。建议澄清 metric 定义：轻量是相对 Python 运行时（成立），还是绝对二进制体积（需测 helio 全家桶链接后体积）。

---

## 六、优点（值得保留）

- **协议层黄金字节流单测兜底**——移植类项目最关键验收手段，抓对了。
- **helio 选型有代码级证据**——引用具体 example 文件和 link target，非凭感觉。
- **三层依赖方向正确**——CLI→data→quotes→proto→helio 无倒置，data 层不碰 FiberSocket，依赖收敛。
- **What We're NOT Building 清晰**——Won't 项明确，避免 HTTP 爬虫/Python 绑定歧义。
- **逐字对照表**——每个协议要点对应上游文件行号，移植有迹可循。
- **Decisions Log 完整**——避免 Architecture by Implication 反模式。

---

## 七、修复优先级与建议动作

| 优先级 | 问题 | 建议动作 | 影响 Phase |
|---|---|---|---|
| P0 | RetryPolicy/CircuitBreaker 错位 | 前移到 Phase 1，修正 MoSCoW 归类 | 重排 Phase 1/3 |
| P0 | helio fiber 纪律传导 | Phase 1 加线程安全审计 + 头文件宏拦截 | Phase 1 Scope |
| P1 | Phase 2/3 并行依据 | 降级声明 + 列黄金字节流 msg_id | Phase 1 Signal |
| P1 | dragonfly 选型 | push back，明确分层建议 | v2 方向锚定 |
| P1 | fiber 异步时序测试 | 增加测试用例类 | 测试策略 |
| P2 | 可观测性/优雅关闭 | Phase 1 加最小日志+drain | Phase 1 Scope |
| P2 | SourceRegistry 抽象 | v1 移除，用具体类 | Phase 3 简化 |
| P2 | null 语义 | 结构体加 optional/sentinel | 数据结构 |
| P2 | datetime Dual Schema | 统一为一种表示 | 数据结构 |
| P2 | "轻量" metric 张力 | 澄清 metric 定义 | Success Metrics |

---

## 八、待用户决策的 Open Questions（APPROVE 前需回答）

1. **RetryPolicy/CircuitBreaker 是否前移到 Phase 1**？（P0-1，决定 Phase 重排）
2. **helio fiber 上下文会调用哪些第三方库**？（iconv/zlib 安全；nlohmann_json/spdlog/fmt 是否在 fiber 内调？）——决定 P0-2 工程手段。
3. **测试代码是否豁免 helio fiber 纪律**？——决定 gtest 写法。
4. **dragonfly 是硬性选型（即便不适合 K线主存储），还是分层方案可接受**？——决定 v2 方向。
5. **Phase 1 黄金字节流测试覆盖哪些 msg_id**？（至少 0x523/0x547/0x0f/0xfc5）

---

## 九、复审建议

修复 P0 两条 + P1 三条后，**只需重读 delta**（Phase 重排段落、Phase 1 Scope 增项、测试策略增项、数据结构定义），不需要全文重审。预期可升至 APPROVE。

---

*下一步：用户确认是否采纳修复建议 → 我应用 P0/P1 修复到 PRD → 复审 → 进入 `/prp-plan` 生成 Phase 1 实施计划。*
