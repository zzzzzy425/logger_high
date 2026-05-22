# logger-high

高性能 C++ 日志库。业务线程同步跑 CPU 密集段（format/compress/encrypt），磁盘 IO 全部异步走 mmap + Strand，shard 自动分片，目录大小自动淘汰，落盘内容用 X25519 + HKDF + AES-256-GCM 加密，可离线解码。

- 语言/标准：C++17
- 平台：Windows / Linux（POSIX）
- 构建：CMake + vcpkg（manifest 模式）
- 依赖（vcpkg 自动拉取）：`zlib` / `zstd` / `protobuf` / `fmt` / `cryptopp`

---

## 1. 整体架构

```
                       业务线程调 LOG_*
                              │
                              ▼
        ┌──────────────────────────────────────────┐
        │                LogHandle                 │  入口（轻量 dispatcher）
        │   等级过滤 + 源位置/时间戳注入 + fan-out  │
        └──────────────────────────────────────────┘
                              │
                ┌─────────────┴─────────────┐
                ▼                           ▼
        ┌───────────────┐         ┌─────────────────────┐
        │  ConsoleSink  │         │   EffectiveSink     │  生产 sink
        │  (调试用，直   │         │   (核心 pipeline)   │
        │   写 stdout)  │         └──────────┬──────────┘
        └───────────────┘                    │ 业务线程同步
                                             ▼
                              ┌──────────────────────────┐
                              │ EffectiveFormatter (proto)│
                              │ ZlibCompressor            │
                              │ AesGcmCipher (AES-256-GCM)│
                              └─────────────┬────────────┘
                                            │ append (memcpy 进 mmap 页)
                                            ▼
                              ┌──────────────────────────┐
                              │  MmapStore (主从多缓冲)   │  双缓冲 + 动态扩容
                              │  · master buffer 收写入   │
                              │  · 写满切换，旧 buffer    │
                              │    投给 Strand 异步 drain │
                              └─────────────┬────────────┘
                                            │ Strand 异步 (借 ThreadPool worker)
                                            ▼
                              ┌──────────────────────────┐
                              │  seal → msync →           │
                              │  ForEachFrame             │
                              │  → ShardWriter.WriteFrame │
                              └─────────────┬────────────┘
                                            │
                                            ▼
                                  log-YYYYMMDD-...-NNNN.bin
                                  (48B 头 + frame 流)

           ┌──────────────────────────────────────────────┐
           │ EvictionRunner  独立线程                     │
           │ 每 30s 扫 log_dir 总量；超 2GiB 删最旧 shard  │
           └──────────────────────────────────────────────┘
```

设计原则：
- **LogHandle 是轻量 dispatcher**：只做等级过滤 + 源位置注入 + 时间戳 + fan-out；不持有重资源
- **能力下沉到 sink**：压缩/加密/mmap/线程池/轮转全部在 EffectiveSink 内部
- **三大插件抽象**：`LogSink`（输出目的地）/ `IFormatter`（序列化格式）/ `RotatePolicy`（轮转策略；当前由 ShardWriter 按大小切，未来抽象）

---

## 2. 一条日志的完整旅程

```
business thread                                  strand worker             eviction thread
─────────────────                                ─────────────             ───────────────
LOG_INFO("hi")
  │
  ▼
LogHandle::Log
  │ ShouldLog? 是
  ▼
EffectiveSink::Log  (pipeline_mu_ 持锁，同一 sink 串行)
  │ 1. EffectiveFormatter::Format     → proto 字节
  │ 2. ZlibCompressor::Compress       → 压缩字节
  │ 3. AesGcmCipher::Encrypt(nonce)   → [12B nonce][密文][16B tag]
  │ 4. 拼帧 [4B LE len][nonce|ct|tag]
  │ 5. MmapStore::Append              → MmapBuffer::Write (lock-free fetch_add)
  ▼                                          │
返回（数据已在 mmap 页，未必落盘）              │ buffer 写满
                                              ▼
                                       SwapMasterLocked_
                                       旧 buffer → strand.PostDetached
                                              │
                                              ▼
                                       Strand 串行 drain：
                                       Seal (等所有写者退出)
                                       Sync (msync 刷脏页)
                                       ForEachFrame:
                                         ShardWriter::WriteFrame
                                           ├─ 当前 shard 写满 → rotate
                                           └─ fwrite 到 log-...bin
                                       ResetForReuse → 还 buffer 池

                                                            每 30s ────►  SweepOnce_
                                                                        遍历 log-*.bin
                                                                        总和 > 2GiB ?
                                                                        是 → fs::remove 最旧
                                                                             (跳过活跃 shard)
```

---

## 3. 线程模型（关键）

整个日志系统在运行期一共开了几个线程？分以下三类：

### 3.1 业务线程（N 个）

**由调用方决定，日志库不创建**。谁调 `LOG_*` 谁就是业务线程。

- 在自己的栈上**同步**执行 format → compress → encrypt → MmapStore.Append
- 同一 `EffectiveSink` 实例上多个业务线程通过 `pipeline_mu_` 串行进入 pipeline（保证四个临时 buffer `buf_proto_ / buf_compressed_ / buf_encrypted_ / buf_frame_` 不被并发访问）
- 进入 MmapBuffer 后改走 lock-free（`fetch_add(offset_)` 抢偏移），写完 memcpy 到 mmap 页就返回
- **不拷贝 LogMsg**，整个热路径零堆分配

### 3.2 ThreadPool worker（[min, max] 个）

由 `logger::context::Context` 全局唯一持有。整个日志库共享 **一个** ThreadPool。

| 参数 | 默认值 |
|---|---|
| `min_threads` | `max(1, hardware_concurrency / 4)` |
| `max_threads` | `hardware_concurrency` |
| `scale_factor` | 2 |
| `idle_timeout` | 30 s |

行为：
- 启动时立刻拉起 `min_threads` 个 worker
- 入队任务数 > `worker_count * scale_factor` 且 < `max_threads` 时扩容一个 worker
- worker 在 `cv_.wait_for(idle_timeout)` 超时 + `worker_count > min_threads` 时自退（自己的 thread 句柄移到 `reap_` vector，下次 enqueue 或 Shutdown 时 join）
- 异常被 worker 吞掉打 stderr，**绝不回调日志库**（防递归）

举例：8 核机器默认 min=2 / max=8，空载只有 2 个 worker；高峰最多扩到 8 个；闲下来 30 s 自动缩回 2 个。

### 3.3 Strand 不开线程

**Strand 是逻辑串行队列，不持有真实线程**。从 `Context::MakeStrand()` 拿到一份后：

- 任务排队进 Strand 内部 `std::queue<Task>`
- 第一个任务到来时 PostDetached 一个 `Drain_` 给 ThreadPool 的**任意 worker** 来跑
- `Drain_` 一直循环取队列任务执行，直到队列空才退出
- `running_` bool（mtx_ 保护）保证同一时刻只有一个 Drain_ 在跑 → 串行性
- 下次有任务进来再 PostDetached 一个新 Drain_

EffectiveSink 持有 **1 个** Strand：所有 MmapStore 的 drain（Seal + msync + ForEachFrame + ShardWriter.WriteFrame）都串行在它上面 → 落盘 IO 永远单线程，没有并发写文件的开销。

### 3.4 EvictionRunner（1 个独立线程）

每个 EffectiveSink 持有 **1 个独立 `std::thread`**，不走 ThreadPool。

- 构造时 `Start()` 拉起，析构时 `Stop()` 通知 cv 并 join
- 循环 `cv_.wait_for(interval)`（默认 30 s），到点扫 `log_dir/log-*.bin`
- 累加大小 > `dir_size_cap`（默认 2 GiB）→ 按字典序删最旧 shard，**跳过当前活跃 shard**（用 `weakly_canonical` 比对路径）
- 异常 `catch + InternalLog warn`，绝不抛

为什么不复用 ThreadPool？淘汰是低频长睡的活，挂在 ThreadPool 里会长期占着一个 worker 名额浪费扩容预算。

### 3.5 线程总数小结（默认配置，1 个 EffectiveSink）

| 角色 | 数量 |
|---|---|
| 业务线程 | N（用户决定） |
| ThreadPool worker | 默认 `[max(1, hw/4), hw]`；8 核机器为 `[2, 8]` |
| Strand 自有线程 | 0（借 ThreadPool 跑） |
| EvictionRunner | 1 |

**8 核机器最小占用 = N + 2 + 1，最大占用 = N + 8 + 1**。

---

## 4. 模块清单

| 目录 | 职责 | 关键类型 |
|---|---|---|
| `handle/` | LogHandle 入口；轻量 dispatcher | `LogHandle` / `LogMsg` / `LOG_*` 宏 |
| `sinks/` | LogSink 抽象 + 具体实现 | `LogSink` / `ConsoleSink` / `EffectiveSink` / `ShardWriter` / `EvictionRunner` |
| `formatter/` | 序列化抽象 + 实现 | `IFormatter` / `default_formatter`（fmt 文本）/ `effective_formatter`（protobuf） |
| `proto/` | 日志 schema | `log_record.proto` → `logger.proto.LogRecord` |
| `compress/` | 压缩抽象 + 实现 | `ICompressor` / `ZlibCompressor` |
| `crypt/` | 加密抽象 + 实现 + 密钥工具 | `ICipher` / `AesGcmCipher` / `X25519KeyAgreement` / `HkdfSha256` / `server_key_loader` |
| `mmap/` | 主从多缓冲 + 帧化追加 | `MmapRegion` / `MmapBuffer` / `MmapStore` |
| `context/` | 线程池 + Strand + 全局单例 | `ThreadPool` / `Strand` / `Context` |
| `utils/` | 公共类型与工具 | `LogLevel` / `SourceLocation` / `process_info` / `internal_log` |
| `examples/` | 端到端冒烟 | `hello` / `context_smoke` / `mmap_smoke` / `compress_smoke` / `crypt_smoke` / `proto_smoke` / `effective_sink_smoke` |
| `third_party/` | 预留（当前空，三方库走 vcpkg） | — |

---

## 5. 构建与运行

### 5.1 环境

- Windows：Visual Studio 2022 + CMake + vcpkg；环境变量 `VCPKG_ROOT` 指向 vcpkg 安装目录
- Linux：gcc ≥ 9 / clang ≥ 10 + cmake ≥ 3.20 + ninja + vcpkg

### 5.2 配置 + 编译

```bash
cmake --preset=default
cmake --build build --config Release
```

vcpkg 第一次会自动下载并编译 `zlib / zstd / protobuf / fmt / cryptopp`，进 `build/vcpkg_installed/`。

### 5.3 跑冒烟

```bash
./build/examples/hello
./build/examples/context_smoke
./build/examples/mmap_smoke
./build/examples/compress_smoke
./build/examples/crypt_smoke
./build/examples/proto_smoke
./build/examples/effective_sink_smoke
```

`effective_sink_smoke` 覆盖 6 个 case：端到端解密、自动分片、淘汰、8 线程并发、背压不死锁、shard header 格式。

---

## 6. 使用示例

```cpp
#include "context/context.h"
#include "handle/log_handle.h"
#include "handle/log_macros.h"
#include "sinks/effective_sink.h"
#include "sinks/effective_sink_config.h"

int main() {
    // 1. 可选：定制线程池
    logger::context::ThreadPool::Config tp_cfg;
    tp_cfg.min_threads = 2;
    tp_cfg.max_threads = 8;
    logger::context::Context::Instance().Configure(tp_cfg);

    // 2. 配置 EffectiveSink
    logger::sinks::EffectiveSinkConfig cfg;
    cfg.log_dir              = "./logs";
    cfg.server_pub_key_path  = "./server_pub.key";  // 32B X25519 raw
    cfg.shard_size_bytes     = 64ull << 20;          // 64 MiB
    cfg.dir_size_cap         = 2ull << 30;           // 2 GiB
    cfg.eviction_interval    = std::chrono::seconds(30);

    auto strand = logger::context::Context::Instance().MakeStrand();
    auto sink   = logger::sinks::EffectiveSink::Create(cfg, strand);

    // 3. 装 sink，开始打
    logger::LogHandle log("app", std::move(sink));
    log.SetLevel(logger::LogLevel::info);

    LOG_INFO(log, "service started, version=%d", 1);
    LOG_WARN(log, "low disk space: {} MiB", 128);

    log.Flush();
    logger::context::Context::Instance().Shutdown();
}
```

注意：
- `EffectiveSink` 的 formatter 在工厂里锁定为 `EffectiveFormatter`，**不要**对它调 `SetFormatter`
- 关闭顺序：先析构 `LogHandle`（连带 sink），再 `Context::Shutdown()`

---

## 7. 文件格式（shard v1）

每个 shard 文件头 48 字节：

```
offset  size  field
  0      8    magic       = "LGRH\x01\x00\x00\x00"
  8      4    algo_id     = LE uint32, 1 = AES-256-GCM + X25519 + HKDF-SHA256
 12     32    client_eph_pub  (32B X25519 raw)
 44      4    reserved    = 0
```

之后是 frame 流，每条：

```
[4B LE length n][n bytes = 12B nonce | ciphertext | 16B tag]   (AES-256-GCM AEAD)
```

文件名 `log-YYYYMMDD-HHMMSS-mmm-NNNN.bin`，字典序 = 时间序，方便淘汰扫描。

---

## 8. 离线解码流程

逻辑全在 `examples/effective_sink_smoke.cpp` 的 Case1：

```
read shard
  ├─ 头 48B → 取 client_eph_pub
  └─ frame 流
       │
       ▼
ECDH(server_priv, client_eph_pub)            // X25519
       │
       ▼
HKDF-SHA256(shared, "", "logger-high aes-256-gcm", 32)
       │
       ▼
AES-256-GCM Decrypt (12B nonce | ct | 16B tag)
       │
       ▼
Zlib Decompress
       │
       ▼
proto::LogRecord::ParseFromString
```

独立的 `logreader` 可执行尚未抽出，欢迎补。

---

## 9. 配置项一览

`EffectiveSinkConfig`（`sinks/effective_sink_config.h`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `log_dir` | — | shard 输出目录（必填） |
| `server_pub_key_path` | — | 审计方 X25519 公钥文件（32B raw，必填） |
| `shard_size_bytes` | 64 MiB | 单个 shard 文件大小上限，超过 rotate |
| `dir_size_cap` | 2 GiB | 目录总量上限，超过 EvictionRunner 删旧 |
| `eviction_interval` | 30 s | 淘汰扫描间隔 |
| `mmap_buffer_bytes` | 4 MiB | 单个 mmap buffer 大小 |
| `mmap_min_buffers` | 2 | mmap 缓冲池下限 |
| `mmap_max_buffers` | 8 | mmap 缓冲池上限（动态扩容到这） |
| `back_pressure_timeout` | 200 ms | 池满时 `Append` 等待新 buffer 的超时，超时返回 false，`InternalLog warn` |
| `zlib_level` | 6 | zlib 压缩级别（1 最快 .. 9 最压） |

`ThreadPool::Config`（`context/thread_pool.h`）：见 §3.2。

---

## 10. 阶段进度

- [x] 阶段 0 地基（CMake / vcpkg / utils）
- [x] 阶段 1 端到端最小切片（LogHandle / ConsoleSink / default_formatter）
- [x] 阶段 2 异步与文件 IO（ThreadPool + Strand + MmapStore 主从双缓冲）
- [x] 阶段 3 数据处理（zlib / AES-GCM + X25519 + HKDF / proto + EffectiveFormatter）
- [x] 阶段 4 核心整合（EffectiveSink + ShardWriter + EvictionRunner，11 个 smoke 全过）
- [ ] 阶段 5+ 生产化：崩溃恢复 / 优雅关停 / dropped 计数器 / 独立 `logreader` / RotatePolicy 抽象 / Key rotation / benchmark / CI
