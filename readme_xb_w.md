# 调用ucx发送活动消息
```c
/* ===========================================
 * Active message handling
 * =========================================== */

nixl_status_t nixlUcxEp::sendAm(unsigned msg_id,
                                void* hdr, size_t hdr_len,
                                void* buffer, size_t len,
                                uint32_t flags, nixlUcxReq &req)
{
    ucs_status_ptr_t request;
    ucp_request_param_t param = {0};

    param.op_attr_mask |= UCP_OP_ATTR_FIELD_FLAGS;
    param.flags         = flags;

    request = ucp_am_send_nbx(eph, msg_id, hdr, hdr_len, buffer, len, &param);

    if (UCS_PTR_IS_PTR(request)) {
        req = (void*)request;
        return NIXL_IN_PROG;
    }

    return ucx_status_to_nixl(UCS_PTR_STATUS(request));
}
```


# nixl调用ucx然后调用verbs的堆栈

你问的 **nixl 在发送 (send) 时调用 UCX 的软件堆栈**，我帮你把整个链路梳理出来。

---

### 1. nixl 的定位

* **nixl** 本身是 vLLM 的分布式通信模块，它不是直接用 `ibv_post_send` 或 RDMA verbs，而是调用 **UCX (Unified Communication X)** 提供的 API。
* UCX 是一层 **通信抽象层**，把 InfiniBand / RoCE / TCP / SHM 等统一起来。

所以 nixl 发送消息时，大体流程是：

```
nixl → UCX UCP 层 (高层 API) → UCX UCT 层 (传输抽象) → UCT transport (ib/rc, ib/ud, tcp, shm) → verbs/libibverbs → NIC
```

---

### 2. nixl 调用 UCX 时的关键路径

以 `send` 为例，通常走的是 **UCP point-to-point** API：

1. **nixl 调用 `ucp_tag_send_nb` / `ucp_am_send_nb`**

   * nixl 内部会调用 UCX 的 *non-blocking send*。
   * UCX 会根据 endpoint 信息决定用哪种传输层（RC, UD, SHM, TCP）。

2. **UCP 层处理**

   * UCP (Unified Communication Protocol) 负责：

     * tag matching / active message 选择
     * fragment 分段和 pipeline
     * rendezvous 协议（大消息时）

3. **UCT 层 (Unified Communication Transport)**

   * UCT 提供抽象接口，比如：

     * `uct_ep_put_zcopy` (RDMA write)
     * `uct_ep_am_bcopy` (Active Message short)
     * `uct_ep_am_zcopy` (Active Message with registered buffer)
   * UCP 会调用这些接口，把上层消息映射成具体传输。

4. **具体 transport (ib/rc, ib/ud, etc.)**

   * 如果是 **IB RC (reliable connection)**：

     * 走 `uct_ib_rc_ep_am_zcopy` → `uct_rc_mlx5_ep_post_send`
     * 最终调用 `mlx5dv_wr_post` → `ibv_post_send`
   * 如果是 **IB UD (unreliable datagram)**：

     * 走 `uct_ib_ud_ep_am_bcopy` → 同样最后 `ibv_post_send`
   * 如果是 **TCP**：

     * 走 `uct_tcp_ep_am_bcopy` → `sendmsg`
   * 如果是 **SHM**：

     * 直接 memcpy + ring buffer

5. **NIC 执行**

   * NIC 通过 doorbell 写入 WQE，执行发送。

---

### 3. UCX 内的几种典型路径

* **小消息**（≤ 8K，一般 inline）：

  * `ucp_am_send_nb` → `uct_ep_am_bcopy` → 直接 copy 到 send buffer → `ibv_post_send`
* **大消息**（> 8K，大约配置相关）：

  * UCX 走 Rendezvous (RNDV)：

    * 先发 RTS (ready to send)，对端发 RTR (ready to receive)
    * 然后走 RDMA PUT/GET → `uct_ep_put_zcopy` / `uct_ep_get_zcopy` → `ibv_post_send`
* **零拷贝**：

  * 如果应用 buffer 已注册内存，走 `uct_ep_am_zcopy` → NIC 直接 DMA。

---

### 4. 一个典型的调用链（send AM zcopy）

```
nixl
 └── ucp_am_send_nb()
      └── ucp_am_send_start()
           └── ucp_do_am_zcopy()
                └── uct_ep_am_zcopy()   # 进入UCT
                     └── uct_rc_mlx5_ep_am_zcopy()
                          └── uct_rc_mlx5_ep_post_send()
                               └── mlx5dv_wr_post()
                                    └── ibv_post_send()
                                         └── NIC doorbell
```

---

✅ 总结：
nixl 的 **发送调用路径** 是
**nixl → UCX UCP → UCT → transport (ib/rc, ud, tcp, shm) → ibv\_post\_send → NIC**。

大消息会触发 **Rendezvous 协议**，小消息直接 inline。

---

要不要我帮你把 **nixl 对应 UCX API 的具体函数调用链**（比如 `nixl::send()` 到 `ucp_am_send_nb()`）画一个详细的 **调用图**？





# 发送接口
def send_notif(




# 后端手册
docs/BackendGuide.md



# 发送接口解读
你贴的这段 **nixl Transfer Operations / Notification Handling** 的接口文档，其实就是在定义一个抽象的 **传输后端 API**，让不同的 backend（UCX, NCCL, MPI, RDMA verbs, TCP …）都能在这个统一接口下被调用。

我帮你逐层解释一下这些函数设计逻辑，以及它们和 UCX 这种软件栈的关系：

---

## 🔹 Transfer Operations

这是数据传输的生命周期管理 API。

### 1. `prepXfer()`

* **输入**：源/目的的 descriptor list（内存块地址+大小），传输方向（读/写），远端 agent 名称。
* **功能**：准备传输所需的上下文，比如注册内存、创建 request handle。
* **产物**：返回一个 `nixlBackendReqH`（后端请求句柄），它是一个抽象基类，具体 backend（例如 UCX）会继承并扩展。
* **类比 UCX**：这里相当于 `ucp_tag_send_nb` / `ucp_put_nb` / `ucp_get_nb` 之前的准备阶段（memh registration + request object setup）。

---

### 2. `estimateXferCost()`

* **输入**：和 `prepXfer` 一样，外加已经生成的 request handle。
* **功能**：估算传输时间（带噪声余量），便于调度层选择最优路径。
* **可选**：不是所有 backend 都需要。
* **类比 UCX**：UCX 自身有性能模型（UCT 层），nixl 可以选择调用或实现一个简化估算函数。

---

### 3. `postXfer()`

* **输入**：request handle。
* **功能**：真正发起传输（asynchronous，不等待完成）。
* **返回**：可能是 `INPROGRESS` 或 `DONE`。小传输可以直接 `DONE`。
* **类比 UCX**：等价于调用 `ucp_put_nb()` / `ucp_tag_send_nb()`，然后返回一个 `ucp_request`。

---

### 4. `checkXfer()`

* **输入**：request handle。
* **功能**：查询传输状态。
* **类比 UCX**：相当于 `ucp_request_check_status()` 或 `ucp_worker_progress()` 之后检查。

---

### 5. `releaseReqH()`

* **输入**：request handle。
* **功能**：释放 request 资源，必要时取消传输（abort）。
* **注意**：必须是非阻塞的。如果底层需要阻塞才能 abort，可以另起线程执行。
* **类比 UCX**：等价于 `ucp_request_free()` 或者 `ucp_request_cancel() + free`。

---

## 🔹 Descriptor List 的并行化

* 一个 transfer request 可以包含多个内存块（跨多个 GPU/NUMA）。
* Backend 可以并行化它们（例如多个 RDMA QP 并行 post send）。
* 传输完成时，**通知（notification）** 只在所有 descriptor 完成时触发一次。

---

## 🔹 Notification Handling

这是一个额外的机制，用于 **传输完成/控制消息** 的通知。

### 1. `getNotifs()`

* **功能**：收集远端发送过来的通知（可能是数据完成、控制信号）。
* **返回**：map\<agent\_name, list of notifications>。
* **类比 UCX**：UCX 本身没有显式通知 API，但可以通过 active message (AM) 或 tag recv 实现。

---

### 2. `genNotif()`

* **功能**：生成一个通知消息发给远端（可能是控制用，比如 barrier ack）。
* **注意**：不是绑定到某个 transfer 的，且无序。
* **类比 UCX**：这就是用 UCX 的 `ucp_am_send_nb()` 发送一个小消息。

---

## 🔹 关键要点

1. **nixl = 一个统一抽象层**

   * 把不同 backend（UCX、RDMA、MPI、NCCL）都包成相同 API。
   * 上层只管调用 `prepXfer → postXfer → checkXfer → releaseReqH`。

2. **UCX 对应关系**

   * `prepXfer` ≈ 内存注册 & request 创建
   * `postXfer` ≈ `ucp_put/get/send_nb`
   * `checkXfer` ≈ `ucp_request_check_status` / poll
   * `releaseReqH` ≈ `ucp_request_free / cancel`
   * `genNotif/getNotif` ≈ `ucp_am_send/recv`

---

要我帮你把这个接口翻译成一个 **伪代码版 UCX backend 实现**（比如 `nixl_ucx_backend.cc`），展示每个函数大概怎么调用 UCX API 吗？



# 参考实现/AI实现
太好啦～下面给你一个**UCX 后端的伪代码实现骨架**（C++-style），把你给的 NIXL 抽象接口完整落到 UCX（UCP/UCT）API 上。代码尽量贴近真实实现（包括内存注册、PUT/GET、AM 通知、progress 线程、可重投 `postXfer()`、异步 `releaseReqH()` 等），你可以据此改成可编译版本。

> 约定：
>
> * “写”=把**本地**数据写到**远端**（RDMA PUT）
> * “读”=从**远端**把数据读到**本地**（RDMA GET）
> * desc = {addr, len, dev\_id}；多 desc 可跨多 GPU/NUMA
> * endpoint/rkey 等通过 NIXL 的 agent metadata/handshake 取得

---

```cpp
// nixl_ucx_backend.hpp  ———— PSEUDOCODE (C++-ish)
// 依赖 UCX: ucp/api/ucp.h, ucs/type/status.h
// 省略了错误检查/日志/资源托管的细节写法，用注释标出关键点

#include <ucp/api/ucp.h>
#include <atomic>
#include <thread>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include <condition_variable>
#include <deque>

// ========== NIXL 抽象类型 ==========
struct Desc {
  void*        addr;      // 本地或远端可访问地址（本地时是HOST/GPUDirect reg 后指针）
  size_t       len;       // 字节数
  int          device_id; // 用于选择 CUDA ctx / NUMA 绑核等（伪代码里不展开）
};

enum class XferOp { READ, WRITE };
enum class ReqState { INIT, INPROGRESS, DONE, ERROR, ABORTING, ABORTED };

struct nixlBackendReqH {
  virtual ~nixlBackendReqH() = default;
};

// ========== UCX 专用结构 ==========
struct RemoteRegion {
  // 远端内存的 rkey；可按 region/desc 维度管理
  ucp_rkey_h rkey = nullptr;
  uint64_t   remote_addr = 0; // 远端虚拟地址（需要在握手时提供）
};

struct RemoteAgentCtx {
  ucp_ep_h ep = nullptr;                    // 到该 agent 的 endpoint
  std::vector<RemoteRegion> regions;       // 对应 desc 列表的 rkey/地址
};

struct Notif {
  std::string from_agent;
  std::vector<uint8_t> payload; // 任意字节
};

// ========== 请求句柄 ==========
struct UcxReq : public nixlBackendReqH {
  XferOp                         op;
  std::string                    remote_agent;
  std::vector<Desc>              local_descs;   // 本地段
  std::vector<size_t>            region_idx;    // 对应远端 region 的索引（与 local_descs 对齐）
  std::vector<ucp_request_param_t> ucp_params;  // 为每个 fragment 准备的参数
  std::vector<void*>             ucp_reqs;      // 每个 fragment 的 UCP request
  std::atomic<ReqState>          state{ReqState::INIT};
  std::atomic<size_t>            completed{0};  // 已完成 fragment 数
  std::atomic<bool>              repost_allowed{true}; // DONE 后允许再次 post
  std::atomic<bool>              abort_flag{false};
  // 可选：起止时间、估算信息等
};

// ========== 后端类 ==========
class NixlUcxBackend {
public:
  struct Options {
    bool supports_notifications = true;
    uint16_t am_notif_id = 0x7F; // Active Message ID for notif
  };

  NixlUcxBackend(const Options& opt) : opt_(opt) {
    init_ucx_();
    spawn_progress_thread_();
    if (opt_.supports_notifications) {
      register_am_handler_();
    }
  }

  ~NixlUcxBackend() {
    running_ = false;
    if (progress_thr_.joinable()) progress_thr_.join();
    // 释放 endpoints、worker、context、rkeys ...（省略）
  }

  //==================== Transfer Ops ====================//

  // prepXfer: 生成 UcxReq，做必要映射（rkey/地址索引），本地内存注册（可选）。
  UcxReq* prepXfer(const std::string& remote_agent,
                   XferOp op,
                   const std::vector<Desc>& local_descs,
                   const std::vector<size_t>& remote_region_index) {
    auto* req = new UcxReq();
    req->op = op;
    req->remote_agent = remote_agent;
    req->local_descs = local_descs;
    req->region_idx = remote_region_index;
    req->ucp_reqs.resize(local_descs.size(), nullptr);
    req->ucp_params.resize(local_descs.size());

    // 这里可做：本地 mem map / cuda memory registration（真实实现必需）
    // 也可把远端 rkey/addr 映射缓存下来（本例在 post 再取）

    req->state.store(ReqState::INIT);
    return req;
  }

  // estimateXferCost: 可选，用链路带宽/RTT/size 做一个粗估
  struct Cost {
    double usec = 0.0;
    double noise_margin = 0.1; // 10%
    const char* method = "BW+RTT model";
  };
  Cost estimateXferCost(UcxReq* req) const {
    size_t bytes = 0;
    for (auto& d : req->local_descs) bytes += d.len;
    // 极简模型：t = RTT + bytes / BW
    // 这里假设 100 Gbps = 12.5 GB/s
    constexpr double GBps = 12.5;
    constexpr double RTT_usec = 2.0;
    double t_usec = RTT_usec + (bytes / (GBps * 1024.0 * 1024.0 * 1024.0)) * 1e6;
    return {.usec = t_usec, .noise_margin = 0.15, .method="toy"};
  }

  // postXfer: 发起真正的 PUT/GET；可多次调用（需上一轮 DONE）
  // 小消息返回后可能立刻 DONE（UCX 可能 inline 完成）
  ReqState postXfer(UcxReq* req) {
    if (req->state.load() == ReqState::INPROGRESS) return req->state.load();
    if (!req->repost_allowed.load()) return ReqState::ERROR; // 防错

    auto it = agents_.find(req->remote_agent);
    if (it == agents_.end()) return ReqState::ERROR;
    auto& agent = it->second;

    req->state.store(ReqState::INPROGRESS);
    req->completed.store(0);
    req->abort_flag.store(false);

    for (size_t i = 0; i < req->local_descs.size(); ++i) {
      const auto& d = req->local_descs[i];
      size_t ridx = req->region_idx[i];
      if (ridx >= agent.regions.size()) { req->state = ReqState::ERROR; break; }
      const auto& rr = agent.regions[ridx];

      ucp_request_param_t prm{};
      prm.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
      prm.cb.send = &NixlUcxBackend::ucx_complete_cb_;
      prm.user_data = this; // 在回调里可以定位到 req/fragment

      req->ucp_params[i] = prm;

      void* ucp_req = nullptr;
      if (req->op == XferOp::WRITE) {
        // RDMA PUT: 本地 d.addr → 远端 rr.remote_addr
        ucp_put_nbx(agent.ep, d.addr, d.len,
                    rr.remote_addr, rr.rkey,
                    &req->ucp_params[i], &ucp_req);
      } else {
        // RDMA GET: 远端 rr.remote_addr → 本地 d.addr
        ucp_get_nbx(agent.ep, d.addr, d.len,
                    rr.remote_addr, rr.rkey,
                    &req->ucp_params[i], &ucp_req);
      }

      // 处理立即完成：ucp_req == NULL => 立刻完成
      if (ucp_req == nullptr) {
        on_fragment_done_(req);
      } else {
        // 需要异步完成：把 (req, fragment_idx, ucp_req) 关联起来
        track_fragment_(ucp_req, req);
      }
    }

    // 可选：如果所有 fragment 都立即完成，state 会已经切到 DONE
    return req->state.load();
  }

  // checkXfer: 查询整体请求状态（注意 progress 线程会推进）
  ReqState checkXfer(UcxReq* req) const {
    return req->state.load();
  }

  // releaseReqH: 异步释放/取消；若无法快速 abort，可标记 ABORTING 并等完成
  // 注意：不可阻塞当前线程
  bool releaseReqH(UcxReq* req) {
    // 尝试取消所有活跃 UCP request（UCX 允许 cancel，可能返回稍后完成）
    req->abort_flag.store(true);

    // 这里不阻塞：交给 progress 线程在回调里做最终 free/回收
    // 我们只是把状态推进到 ABORTING/ABORTED
    auto s = req->state.load();
    if (s == ReqState::INPROGRESS) {
      req->state.store(ReqState::ABORTING);
      // 实际上你可以调用 ucp_request_cancel(worker_, ucp_req)，
      // 在回调里识别被取消并计数完成
      return false; // 表示仍在 abort 过程中
    }

    // DONE/ERROR/ABORTED：说明已无未完成 fragment，可真正释放
    // 真正 delete 通常由上层调用者做；此处仅返回成功
    req->state.store(ReqState::ABORTED);
    return true;
  }

  //==================== Notifications ====================//

  // getNotifs: 拉取收到的通知
  std::unordered_map<std::string, std::vector<Notif>> getNotifs() {
    std::lock_guard<std::mutex> lk(notif_mu_);
    auto out = std::move(notif_buf_);
    notif_buf_.clear();
    return out;
  }

  // genNotif: 发送一个 AM 小消息（与传输无绑定、无序保证）
  bool genNotif(const std::string& to_agent, const void* data, size_t len) {
    if (!opt_.supports_notifications) return false;
    auto it = agents_.find(to_agent);
    if (it == agents_.end()) return false;
    ucp_request_param_t prm{};
    prm.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK | UCP_OP_ATTR_FIELD_USER_DATA;
    prm.cb.send = &NixlUcxBackend::ucx_am_send_complete_cb_;
    prm.user_data = this;
    void* req = nullptr;
    ucp_am_send_nbx(it->second.ep, opt_.am_notif_id, nullptr, 0, data, len, &prm, &req);
    if (req == nullptr) return true; // 立即完成
    // 非阻塞，真正完成由回调推进
    return true;
  }

  //==================== Agent/资源管理（示意） ====================//

  // 在握手阶段由 NIXL 上层调用：完成 ep 建立、rkey unpack
  bool addRemoteAgent(const std::string& name, const ucp_ep_params_t& ep_params,
                      const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& remote_regions)
  {
    ucp_ep_h ep{};
    if (ucp_ep_create(worker_, &ep_params, &ep) != UCS_OK) return false;

    RemoteAgentCtx ctx;
    ctx.ep = ep;
    for (auto& r : remote_regions) {
      RemoteRegion rr;
      rr.remote_addr = r.first;
      // r.second 是 rkey buffer，需要 unpack
      if (ucp_ep_rkey_unpack(ep, r.second.data(), &rr.rkey) != UCS_OK) return false;
      ctx.regions.push_back(rr);
    }
    agents_[name] = std::move(ctx);
    return true;
  }

private:
  //==================== UCX 初始化/进展推进 ====================//
  void init_ucx_() {
    // 创建 context/worker（省略细节/参数）
    ucp_params_t params{};
    params.field_mask = UCP_PARAM_FIELD_FEATURES;
    params.features = UCP_FEATURE_RMA | UCP_FEATURE_AM;
    ucp_config_t* cfg;
    ucp_config_read(nullptr, nullptr, &cfg);
    ucp_init(&params, cfg, &context_);
    ucp_config_release(cfg);

    ucp_worker_params_t wprm{};
    wprm.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    wprm.thread_mode = UCS_THREAD_MODE_MULTI;
    ucp_worker_create(context_, &wprm, &worker_);
  }

  void spawn_progress_thread_() {
    running_ = true;
    progress_thr_ = std::thread([this]{
      while (running_) {
        // 推进完成事件
        ucp_worker_progress(worker_);
        // 处理已完成 fragment 的 request 状态推进
        drain_completions_();
        // 轻微让出
        std::this_thread::yield();
      }
    });
  }

  //==================== 完成/回调绑定 ====================//
  struct FragDone {
    UcxReq* req;
    // 可加 fragment 索引等
  };

  static void ucx_complete_cb_(void* request, ucs_status_t status, void* user_data) {
    auto* self = static_cast<NixlUcxBackend*>(user_data);
    // UCX 允许在 cb 里 free 或延后 free；我们把 request 指针入队，统一处理
    {
      std::lock_guard<std::mutex> lk(self->comp_mu_);
      self->completed_queue_.push_back(request);
      self->completed_status_.push_back(status);
    }
  }

  static void ucx_am_recv_cb_(void* arg, const void* header, size_t header_length,
                              void* data, size_t length, const ucp_am_recv_param_t* param) {
    auto* self = static_cast<NixlUcxBackend*>(arg);
    // 这里实际需要从 header 或 payload 中解析出 from_agent
    Notif n;
    n.from_agent = self->decode_from_agent_(header, header_length, data, length);
    n.payload.assign((const uint8_t*)data, (const uint8_t*)data + length);
    {
      std::lock_guard<std::mutex> lk(self->notif_mu_);
      self->notif_buf_[n.from_agent].push_back(std::move(n));
    }
    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_DATA) {
      ucp_am_data_release(self->worker_, data);
    }
  }

  static void ucx_am_send_complete_cb_(void* request, ucs_status_t status, void* user_data) {
    // 这里通常直接 free
    ucp_request_free(request);
  }

  void register_am_handler_() {
    ucp_am_handler_param_t p{};
    p.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                   UCP_AM_HANDLER_PARAM_FIELD_CB |
                   UCP_AM_HANDLER_PARAM_FIELD_ARG;
    p.id   = opt_.am_notif_id;
    p.cb   = &NixlUcxBackend::ucx_am_recv_cb_;
    p.arg  = this;
    ucp_am_set_handler(worker_, &p);
  }

  void track_fragment_(void* ucp_req, UcxReq* req) {
    std::lock_guard<std::mutex> lk(track_mu_);
    req_of_ucp_req_[ucp_req] = req;
  }

  void on_fragment_done_(UcxReq* req) {
    size_t done = req->completed.fetch_add(1) + 1;
    if (done == req->local_descs.size()) {
      // 全部 fragment 完成
      if (req->abort_flag.load()) {
        req->state.store(ReqState::ABORTED);
      } else {
        req->state.store(ReqState::DONE);
      }
      // 允许重投
      req->repost_allowed.store(true);
    }
  }

  void drain_completions_() {
    std::deque<void*> local;
    std::deque<ucs_status_t> sts;
    {
      std::lock_guard<std::mutex> lk(comp_mu_);
      local.swap(completed_queue_);
      sts.swap(completed_status_);
    }
    while (!local.empty()) {
      void* ucp_req = local.front(); local.pop_front();
      ucs_status_t st = sts.front(); sts.pop_front();

      UcxReq* req = nullptr;
      {
        std::lock_guard<std::mutex> lk(track_mu_);
        auto it = req_of_ucp_req_.find(ucp_req);
        if (it != req_of_ucp_req_.end()) {
          req = it->second;
          req_of_ucp_req_.erase(it);
        }
      }
      ucp_request_free(ucp_req);

      if (!req) continue;
      if (st == UCS_OK) {
        on_fragment_done_(req);
      } else {
        req->state.store(ReqState::ERROR);
      }
    }
  }

  std::string decode_from_agent_(const void* hdr, size_t hlen, const void* data, size_t len) {
    // 真实实现里：从 header 编码出 agent 名称；此处返回占位
    return "unknown";
  }

private:
  Options opt_;
  ucp_context_h context_{};
  ucp_worker_h  worker_{};

  std::unordered_map<std::string, RemoteAgentCtx> agents_;

  std::atomic<bool> running_{false};
  std::thread progress_thr_;

  // 完成事件缓冲
  std::mutex comp_mu_;
  std::deque<void*> completed_queue_;
  std::deque<ucs_status_t> completed_status_;
  std::mutex track_mu_;
  std::unordered_map<void*, UcxReq*> req_of_ucp_req_;

  // 通知缓冲
  std::mutex notif_mu_;
  std::unordered_map<std::string, std::vector<Notif>> notif_buf_;
};
```

---

## 关键对照表（NIXL ↔ UCX）

| NIXL API           | UCX 对应                                                        | 说明                            |
| ------------------ | ------------------------------------------------------------- | ----------------------------- |
| `prepXfer`         | `ucp_rkey_unpack`、本地 mem map、构造 request                       | 绑定远端 rkey/addr，准备本地注册内存       |
| `estimateXferCost` | （自定义）                                                         | 可用带宽/RTT 模型或 UCX 统计           |
| `postXfer`         | `ucp_put_nbx` / `ucp_get_nbx` / `ucp_am_send_nbx`             | 非阻塞；小消息可能立即完成                 |
| `checkXfer`        | `ucp_worker_progress` + `ucp_request_check_status`（这里在进展线程推进） | 返回 DONE/INPROGRESS/ERROR      |
| `releaseReqH`      | `ucp_request_cancel` + `ucp_request_free`                     | 要求“非阻塞”；可进入 ABORTING          |
| `genNotif`         | `ucp_am_send_nbx`                                             | 发送控制/完成通知                     |
| `getNotifs`        | `ucp_am_set_handler` 回调收包 → 本地队列                              | 返回 map\<agent, notifications> |

---

## 使用示意（伪代码）

```cpp
NixlUcxBackend::Options opt;
opt.supports_notifications = true;
opt.am_notif_id = 42;
NixlUcxBackend be(opt);

// …… 握手阶段：建立 ep、unpack rkeys ……
be.addRemoteAgent("prefill-0", ep_params, remote_regions);

// 组织一次跨多个 block 的 READ：
std::vector<Desc> locals = {
  {dst_ptr0, len0, /*dev*/0},
  {dst_ptr1, len1, /*dev*/0},
};
std::vector<size_t> rids = {0, 1}; // 远端 region 索引（与 locals 对齐）

auto* h = be.prepXfer("prefill-0", XferOp::READ, locals, rids);
be.postXfer(h);

// 轮询状态（真实里通常在调度主循环里做）
while (be.checkXfer(h) == ReqState::INPROGRESS) {
  /* do other work */
}
// 完成后允许重投
be.postXfer(h); // 如果你要复用同一批 desc，再次读取

// 释放（异步）
be.releaseReqH(h);
delete h;
```

---

### 实装提示

* **GPU/GPUDirect**：真实实现要在 `prepXfer()` 里做 CUDA ctx 绑定和 `ucp_mem_map`（可选），并确保内存 pin/注册；UCX + GDR 需编译选项和运行时检查。
* **rkey/addr 管理**：建议按 *layer × block* 或 *region* 编址，保证与 NIXL desc 列表一致。
* **ABORT 语义**：如果底层取消代价大，遵循文档建议：`releaseReqH()` 立即返回“未成功”，置位 ABORTING；最终由 progress 线程回调推进到 ABORTED。
* **通知**：如果后端不支持通知（`supports_notifications=false`），NIXL 上层就不会要求 `genNotif/getNotifs`。支持时推荐用 **Active Message**（AM）通道。
* **可重投**：确保只有在 `DONE` 后才允许再次 `postXfer()`；否则返回错误状态。
* **并行化**：desc 级别可以开多路 QP/并行 post（这里用最简队列示意）。

如果你愿意，我可以把这个“伪代码骨架”扩成**可编译的最小 UCX 示例**（含 CMake、两进程跑 PUT/GET+AM 通知），或者按你现有的 NIXL wrapper 接口改名对齐。




# ucx实现发送语义
nixl_status_t nixlUcxEngine::postXfer
case NIXL_READ:
    ret = ep->read(raddr, rmd->getRkey(workerId), laddr, lmd->mem, lsize, req);
    break;
case NIXL_WRITE:
    ret = ep->write(laddr, lmd->mem, raddr, rmd->getRkey(workerId), lsize, req)
    break;


# write实现
ucs_status_ptr_t request = ucp_put_nbx(eph, laddr, size, raddr, rkey.get(), &param);
ucx_status_to_nixl(UCS_PTR_STATUS(request))


# read实现
ucs_status_ptr_t request = ucp_get_nbx(eph, laddr, size, raddr, rkey.get(), &param);




# ucx接口实现:
src/plugins/ucx/ucx_backend.h

