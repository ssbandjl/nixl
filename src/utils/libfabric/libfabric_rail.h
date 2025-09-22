/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2025 Amazon.com, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_RAIL_H
#define NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_RAIL_H

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <ostream>
#include <stack>

#include "nixl.h"
#include "backend/backend_aux.h"
#include "libfabric/libfabric_common.h"

// Forward declarations
class nixlLibfabricConnection;

/**
 * @brief Request structure for libfabric operations
 *
 */
struct nixlLibfabricReq {
    fi_context2 ctx; ///< Libfabric context for operation tracking
    size_t rail_id; ///< Rail ID that owns this request
    uint32_t xfer_id; ///< Pre-assigned globally unique transfer ID
    void *buffer; ///< Pre-assigned buffer for CONTROL operations, nullptr for DATA
    struct fid_mr *mr; ///< Pre-assigned memory registration for CONTROL, nullptr for DATA
    size_t buffer_size; ///< Pre-assigned buffer size for CONTROL (2KB), 0 for DATA

    enum OpType { WRITE, READ, SEND, RECV } operation_type; ///< Operation type (pre-assigned)

    bool in_use; ///< Pool management flag
    size_t chunk_offset; ///< Chunk offset for DATA requests
    size_t chunk_size; ///< Chunk size for DATA requests
    std::function<void()> completion_callback; ///< Completion callback function
    void *local_addr; ///< Local memory address for transfers
    uint64_t remote_addr; ///< Remote memory address for transfers
    struct fid_mr *local_mr; ///< Local memory registration for transfers
    uint64_t remote_key; ///< Remote access key for transfers

    /** Default constructor initializing all fields */
    nixlLibfabricReq()
        : rail_id(0),
          xfer_id(0),
          buffer(nullptr),
          mr(nullptr),
          buffer_size(0),
          operation_type(SEND),
          in_use(false),
          chunk_offset(0),
          chunk_size(0),
          local_addr(nullptr),
          remote_addr(0),
          local_mr(nullptr),
          remote_key(0) {
        memset(&ctx, 0, sizeof(fi_context2));
    }
};

/** Thread-safe request pool with O(1) allocation/release */
class RequestPool {
public:
    /** Initialize request pool with specified size */
    RequestPool(size_t pool_size, size_t rail_id);

    /** Virtual destructor for proper cleanup */
    virtual ~RequestPool() = default;

    /** Release request back to the pool */
    virtual void
    release(nixlLibfabricReq *req);

    /** Find request by libfabric context pointer */
    nixlLibfabricReq *
    findByContext(void *context) const;

    /** Get count of currently active requests */
    size_t
    getActiveRequestCount() const;

    // Non-copyable and non-movable since we use unique_ptr for management
    RequestPool(const RequestPool &) = delete;
    RequestPool &
    operator=(const RequestPool &) = delete;
    RequestPool(RequestPool &&) = delete;
    RequestPool &
    operator=(RequestPool &&) = delete;

protected:
    std::vector<nixlLibfabricReq> requests_; ///< Fixed-size request pool
    std::stack<size_t> free_indices_; ///< Stack of available request indices
    size_t rail_id_; ///< Rail ID for this pool
    mutable std::mutex pool_mutex_; ///< Thread safety protection
};

/** Control request pool with pre-allocated buffers for SEND/RECV operations */
class ControlRequestPool : public RequestPool {
public:
    /** Initialize control request pool */
    ControlRequestPool(size_t pool_size, size_t rail_id);

    /** Destructor with explicit cleanup */
    ~ControlRequestPool();

    // Non-copyable and non-movable since we use unique_ptr for management
    ControlRequestPool(const ControlRequestPool &) = delete;
    ControlRequestPool &
    operator=(const ControlRequestPool &) = delete;
    ControlRequestPool(ControlRequestPool &&) = delete;
    ControlRequestPool &
    operator=(ControlRequestPool &&) = delete;

    /** Initialize pool with buffers and pre-assigned XFER_IDs */
    nixl_status_t
    initializeWithBuffersAndXferIds(struct fid_domain *domain,
                                    const std::vector<uint32_t> &xfer_ids);

    /** Allocate control request with size validation */
    nixlLibfabricReq *
    allocate(size_t needed_size);

    /** Explicit cleanup method for proper resource ordering */
    void
    cleanup();

    /** Buffer size constant for validation */
    static constexpr size_t BUFFER_SIZE = NIXL_LIBFABRIC_SEND_RECV_BUFFER_SIZE;

private:
    void *buffer_chunk_; ///< Large pre-registered buffer chunk
    size_t buffer_chunk_size_; ///< Total size of buffer chunk
    struct fid_mr *buffer_mr_; ///< Memory registration for chunk
};

/** Lightweight data request pool for WRITE/READ operations */
class DataRequestPool : public RequestPool {
public:
    /** Initialize data request pool */
    DataRequestPool(size_t pool_size, size_t rail_id);

    /** Default destructor (no special cleanup needed) */
    ~DataRequestPool() = default;

    // Non-copyable and non-movable since we use unique_ptr for management
    DataRequestPool(const DataRequestPool &) = delete;
    DataRequestPool &
    operator=(const DataRequestPool &) = delete;
    DataRequestPool(DataRequestPool &&) = delete;
    DataRequestPool &
    operator=(DataRequestPool &&) = delete;

    /** Initialize pool with pre-assigned XFER_IDs */
    nixl_status_t
    initializeWithXferIds(const std::vector<uint32_t> &xfer_ids);

    /** Allocate data request for specified operation type */
    nixlLibfabricReq *
    allocate(nixlLibfabricReq::OpType op_type);
};


/** Connection state tracking for multi-rail connections */
enum class ConnectionState {
    DISCONNECTED, ///< No connection attempt made, initial state
    CONNECT_REQ_SENT, ///< Connection request sent, waiting for ACK
    CONNECT_ACK_SENT, ///< Connection ACK sent (target side)
    CONNECTED, ///< ACK received, ready for data transfers
    FAILED ///< Connection attempt failed
};

// Stream operator for ConnectionState to enable logging
inline std::ostream &
operator<<(std::ostream &os, const ConnectionState &state) {
    switch (state) {
    case ConnectionState::DISCONNECTED:
        return os << "DISCONNECTED";
    case ConnectionState::CONNECT_REQ_SENT:
        return os << "CONNECT_REQ_SENT";
    case ConnectionState::CONNECT_ACK_SENT:
        return os << "CONNECT_ACK_SENT";
    case ConnectionState::CONNECTED:
        return os << "CONNECTED";
    case ConnectionState::FAILED:
        return os << "FAILED";
    default:
        return os << "UNKNOWN";
    }
}

/** Individual libfabric rail managing fabric, domain, endpoint, CQ, and AV */
class nixlLibfabricRail {
public:
    uint16_t rail_id; ///< Unique rail identifier
    std::string device_name; ///< EFA device name for this rail
    char ep_name[LF_EP_NAME_MAX_LEN]; ///< Endpoint name for connection setup
    mutable bool blocking_cq_sread_supported; ///< Whether blocking CQ reads are supported
    struct fid_ep *endpoint; ///< Libfabric endpoint handle

    /** Initialize libfabric rail with all resources */
    nixlLibfabricRail(const std::string &device, uint16_t id);

    /** Destroy rail and cleanup all libfabric resources */
    ~nixlLibfabricRail();

    // Non-copyable and non-movable since we use unique_ptr for management
    nixlLibfabricRail(const nixlLibfabricRail &) = delete;
    nixlLibfabricRail &
    operator=(const nixlLibfabricRail &) = delete;
    nixlLibfabricRail(nixlLibfabricRail &&) = delete;
    nixlLibfabricRail &
    operator=(nixlLibfabricRail &&) = delete;

    /** Explicit cleanup method for proper resource ordering */
    void
    cleanup();

    /** Validate that rail is properly initialized */
    bool
    isProperlyInitialized() const;

    // Memory registration methods
    /** Register memory buffer with libfabric */
    nixl_status_t
    registerMemory(void *buffer,
                   size_t length,
                   uint64_t access_flags,
                   struct fid_mr **mr_out,
                   uint64_t *key_out) const;

    /** Deregister memory from libfabric */
    nixl_status_t
    deregisterMemory(struct fid_mr *mr) const;

    // Address vector management methods
    /** Insert remote endpoint address into address vector */
    nixl_status_t
    insertAddress(const void *addr, fi_addr_t *fi_addr_out) const;

    /** Remove address from address vector */
    nixl_status_t
    removeAddress(fi_addr_t fi_addr) const;

    // Memory descriptor helper methods
    /** Get libfabric memory descriptor for MR */
    void *
    getMemoryDescriptor(struct fid_mr *mr) const;

    /** Get remote access key for MR */
    uint64_t
    getMemoryKey(struct fid_mr *mr) const;

    // Libfabric operation wrappers
    /** Post receive operation */
    nixl_status_t
    postRecv(nixlLibfabricReq *req) const;

    /** Post send operation with immediate data */
    nixl_status_t
    postSend(uint64_t immediate_data, fi_addr_t dest_addr, nixlLibfabricReq *req) const;

    /** Post RDMA write operation with immediate data */
    nixl_status_t
    postWrite(const void *local_buffer,
              size_t length,
              void *local_desc,
              uint64_t immediate_data,
              fi_addr_t dest_addr,
              uint64_t remote_addr,
              uint64_t remote_key,
              nixlLibfabricReq *req) const;

    /** Post RDMA read operation */
    nixl_status_t
    postRead(void *local_buffer,
             size_t length,
             void *local_desc,
             fi_addr_t dest_addr,
             uint64_t remote_addr,
             uint64_t remote_key,
             nixlLibfabricReq *req) const;

    /** Process completion queue with batching support */
    nixl_status_t
    progressCompletionQueue(bool use_blocking = false);

    // Callback registration methods
    /** Set callback for notification message processing */
    void
    setNotificationCallback(std::function<void(const std::string &)> callback);

    /** Set callback for connection acknowledgment processing */
    void
    setConnectionAckCallback(
        std::function<void(uint16_t, nixlLibfabricConnection *, ConnectionState)> callback);

    /** Set callback for connection request processing */
    void
    setConnectionReqCallback(
        std::function<nixl_status_t(uint16_t, const std::string &, nixlLibfabricRail *)> callback);

    /** Set callback for XFER_ID tracking */
    void
    setXferIdCallback(std::function<void(uint32_t)> callback);

    // Optimized resource management methods
    /** Allocate control request with size validation */
    [[nodiscard]] nixlLibfabricReq *
    allocateControlRequest(size_t needed_size);

    /** Allocate data request for specified operation */
    [[nodiscard]] nixlLibfabricReq *
    allocateDataRequest(nixlLibfabricReq::OpType op_type);

    /** Release request back to appropriate pool */
    void
    releaseRequest(nixlLibfabricReq *req);

    /** Find request from libfabric context pointer */
    nixlLibfabricReq *
    findRequestFromContext(void *context) const;

private:
    // Core libfabric resources
    struct fi_info *info; // from rail_infos[rail_id]
    struct fid_fabric *fabric; // from rail_fabrics[rail_id]
    struct fid_domain *domain; // from rail_domains[rail_id]
    struct fid_cq *cq; // from rail_cqs[rail_id]
    struct fid_av *av; // from rail_avs[rail_id]

    // CQ progress mutex to protect completion queue operations
    mutable std::mutex cq_progress_mutex_;

    // Callback functions
    std::function<void(const std::string &)> notificationCallback;
    std::function<void(uint16_t, nixlLibfabricConnection *, ConnectionState)> connectionAckCallback;
    std::function<nixl_status_t(uint16_t, const std::string &, nixlLibfabricRail *)>
        connectionReqCallback;
    // XFER_ID tracking callback
    std::function<void(uint32_t)> xferIdCallback;

    // Separate request pools for optimal performance
    ControlRequestPool
        control_request_pool_; // 256 CONTROL requests (SEND/RECV) with internal buffers
    DataRequestPool data_request_pool_; // 1024 DATA requests (WRITE/read) - no buffers needed

    // Configuration constants
    static constexpr size_t CONTROL_REQUESTS_PER_RAIL =
        256; // SEND/RECV operations (1:1 with buffers)
    static constexpr size_t DATA_REQUESTS_PER_RAIL = 1024; // WRITE/read operations (no buffers)

    nixl_status_t
    processCompletionQueueEntry(struct fi_cq_data_entry *comp);
    nixl_status_t
    processLocalSendCompletion(struct fi_cq_data_entry *comp);
    nixl_status_t
    processLocalTransferCompletion(struct fi_cq_data_entry *comp, const char *operation_type);
    nixl_status_t
    processRecvCompletion(struct fi_cq_data_entry *comp);
    nixl_status_t
    processRemoteWriteCompletion(struct fi_cq_data_entry *comp) const;
};


#endif // NIXL_SRC_UTILS_LIBFABRIC_LIBFABRIC_RAIL_H
