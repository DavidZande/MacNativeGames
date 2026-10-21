# Mac 性能待做与复测方法

本页保留原开发树 `native-port/TODO.md`、`docs/testing-methods.md` 中 2026-10-17 的重要结论，并与本工具的 [1.4.3 记录](v1.4.3-port.md) 分开。旧实验的开关、源码和二进制须按具体分支核对，不能把旧版通过或未合入的实验当作当前 1.4.3 验收。这里只安排 Mac；不代表 iPhone 已测。

## Current local status — 2026-10-19

The consolidated [practical findings](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/practical-performance-20261019.md) now cover
David's M1 Pro / TOTK 1.0.0 work, with links to raw evidence, binary identities,
the packaged-app state and remaining work. This is separate from the historical
M4 / 1.4.3 results below.

- Tests are now uncapped renderer-throughput measurements with no FPS targets.
  The benchmark window shows phases and still previews, not live presentation.
  [Current commands and methodology](uncapped-benchmarks.md).
- The tested 900p world is primarily GPU-limited, around 83–84 FPS standing.
  720p + MetalFX reached 92.02 FPS, with a resolution/quality tradeoff.
- Allocation logging was removed from the default path without a demonstrated
  steady FPS gain. Video mapping batching and non-GPU stall fixes remain open.
- [Expanded compute warm-up](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/compute-pipeline-warmup.md) added startup cost;
  its extra pipeline coverage was not exercised by the route. It remains off.
- [Binding caches](https://github.com/DavidZande/MacNativeGames/blob/notes-2026-10-21/docs/binding-fastpaths-experiment.md) passed correctness checks
  but showed no reliable FPS improvement in quick A/B tests. Both remain off.
- GPU timestamp capture now identifies individual pass candidates. The guest
  event-pacing loop was traced only at startup and is not a demonstrated cause
  of gameplay stalls. Uniform-copy reduction and further parallel replay are
  hypotheses rather than implemented or measured wins.

## 已有实验应保留的结论

旧版 M4 / macOS 26.5.2、监视堡垒存档副本，取消显示节拍限制的渲染能力测试；不是实际上屏 FPS。每组同一二进制，8 个 30 秒平衡交替窗口。下表是每条件 4 个窗口分位数的算术均值，不是合并帧分位数；天气和时间仍自然变化。

| 旧实验 | 基线 p50 / p95（ms） | 实验 p50 / p95（ms） | 结论 |
|---|---|---|---|
| 同次绘制准备结果复用 | 11.839 / 12.624 | 11.904 / 12.661 | 未发现收益，p50 慢约 0.55% |
| 复制批处理 | 11.741 / 12.549 | 11.727 / 12.516 | 实际优化 ops/barriers/saved 均为 0，性能未覆盖 |
| 500 µs 提交预算 | 11.690 / 12.517 | 11.636 / 12.497 | 约 0.46% 差异不足以启用 |

后续 5 组提交预算的 p50 变化约 −0.68% 到 +0.28%，仍没有可靠收益；保留首段 60 / 后续 75 draw 的默认策略。全段较小预算可以让 GPU 更早开工，却增加后续空档，不能只看首次开工就称 GPU 等待已解决。

旧成本计数：编码阶段 resource-ID 查询 99.46% 为快路，约 11 ns；准备阶段快路约 90.22%，慢路约 566 ns。这些包括计量开销、只计持锁后的局部解析，不含锁等待或整个函数，不能直接乘调用数换算提帧。

旧实验树对读后写、写后读、写后写及别名做过受控 GPU 验证：普通 Device 屏障不足以覆盖 `mach_vm_remap` / 多个 no-copy buffer 包装的别名，曾需要 ResourceAlias 可见性。该正确性结论应保留；移植到本工具的具体路径仍须独立核查和正反例验证，不能用另一个树的通过记录代替。fill 的 helper 覆盖也不等于真实游戏 fill 路径已覆盖。

## 后续优先级

1. **当前 1.0.0 的 GPU 通道与 shader。** 从已采集的逐通道排名确认完整 draw/shader 负载，再减少可证明冗余的工作；保持分辨率与画质。已有 pass merge / store look-ahead 不能重复计为新优化。探针会改变提交方式，其 GPU 时间不能冒充正常路径的 FPS。
2. **非 GPU 长帧。** 71.9 / 85.4 ms 长帧当时 GPU 约 12 ms、无新管线编译；需按帧号关联调度、同步和其他宿主工作，不能先认定原因。参考检查、采样及截图与正式测速分窗。
3. **准备阶段失效和实际复制负载。** 简单 argument epoch / texture plan 已快速 A/B，暂无启用依据。细粒度失效和每帧约 250 次 uniform 副本仍需证明冗余后再改；保留上传、别名切换、uniform 快照、GPU 写回和完成寿命。buffer copy / update fallback 计数非零才算覆盖，不能关闭已有优化制造命中。
4. **长期内存。** 区分游戏堆、序号表、贴图池槽位、管线/驱动缓存、驻留集合与 GPU 完成后的实际对象。`mtl_dead_forget` 当前保守保留已销毁序号；缩表前须补齐槽位引用关系，不能重现旧的过早遗忘错误。1.4.3 的补帧配置复用只针对重复构建，其他增长仍须归因。
5. **首次编译卡顿。** 同路线分别记录冷/热缓存，收集完整帧时间。相对尖峰日志会漏掉普通慢帧；截图 FPS、固定镜头平均值、补帧数都不能证明稳定 30/60。
6. **场景、动作及真实显示器。** 复杂能力/战斗/过场、菜单传送与跨场景资源退役、长时间快速切档、物理 4K 和高刷新率分别检查。版本对比必须同机同负载，不比较不同游戏/设备的单张 FPS。
7. **其他版本单独建基线。** 重测 1.4.3 时固定构建、存档、镜头、内部/输出尺寸、缓存和呈现方式；本次 1.0.0 结果不能替代。

## 每轮测试的最低记录

- 记录提交、未提交 diff、新增源码、实际二进制 SHA-256、所选版本/资源哈希和环境；只有 commit 名不够。
- 使用独立 profile 和唯一共享 game-slot，用户进程优先；禁止两份游戏/GPU 探针并行。
- 正式性能结论采用至少 8 × 30 秒 A/B/B/A/B/A/A/B，保留全部慢帧。用户要求快速筛查时可用较短窗口；如本次 4 × 8/10 秒只用于筛除无可靠收益的候选，不据此宣称稳定提升。采样、参考解析核对、截图和重型诊断与测速分窗。
- 区分世界、菜单、加载和过场；记录真实单调时间、帧号、位置/视角、天气和缓存变化。没有严格固定的变量写明限制。
- 报告完整帧分布的 p50/p95/p99/最大值和超预算比例；不要平均分位数后当作合并结果，也不要将重叠 CPU/GPU 时间相加。
- 同时核对实际画面及执行错误。复制组记录 ops/barriers/saved 差值；计数为零就是未覆盖。
- 内存分别保留 RSS、footprint、进程峰值、Metal 分配、驻留对象及完成后释放；一个总数不能证明泄漏或平台期。重型诊断记录时间窗口。
- 通过、无收益、未覆盖、负对照失败和被中断的测试分别记录；不为追求分数删掉卡顿或异常。

旧版代表场景 p95≤10 ms 只是历史阶段目标；当前用户要求 benchmark 无 FPS 达标门槛，按完整帧分布比较吞吐、卡顿和资源成本。1.4.3 仍须建立自己的基线。单项正确性修复、版本适配和内存改进也应分别交代，不能都称为 FPS 提升。
