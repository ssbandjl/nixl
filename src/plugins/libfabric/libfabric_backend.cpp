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

#include "libfabric_backend.h"
#include "serdes/serdes.h"
#include "common/nixl_log.h"
#include "libfabric/libfabric_topology.h"

#include <limits>
#include <cstring>
#include <unistd.h>

#include <iomanip>
#include <numeric>

#include "absl/strings/numbers.h"

#ifdef HAVE_CUDA
// CUDA error checking macros
#define CHECK_CUDA_ERROR(result, message)                                                         \
    do {                                                                                          \
        if (result != cudaSuccess) {                                                              \
            NIXL_ERROR << "CUDA Error: " << message << " (" << cudaGetErrorString(result) << ")"; \
            return NIXL_ERR_BACKEND;                                                              \
        }                                                                                         \
    } while (0)

#define CHECK_CUDA_DRIVER_ERROR(result, message)                                        \
    do {                                                                                \
        if (result != CUDA_SUCCESS) {                                                   \
            const char *error_str;                                                      \
            cuGetErrorString(result, &error_str);                                       \
            NIXL_ERROR << "CUDA Driver Error: " << message << " (" << error_str << ")"; \
            return NIXL_ERR_BACKEND;                                                    \
        }                                                                               \
    } while (0)
#endif

/****************************************
 * CUDA Context Management
 *****************************************/

#ifdef HAVE_CUDA
static int
cudaQueryAddr(void *address, bool &is_dev, CUdevice &dev, CUcontext &ctx) {
    CUmemorytype mem_type = CU_MEMORYTYPE_HOST;
    uint32_t is_managed = 0;
    CUpointer_attribute attr_type[4];
    void *attr_data[4];
    CUresult result;

    attr_type[0] = CU_POINTER_ATTRIBUTE_MEMORY_TYPE;
    attr_data[0] = &mem_type;
    attr_type[1] = CU_POINTER_ATTRIBUTE_IS_MANAGED;
    attr_data[1] = &is_managed;
    attr_type[2] = CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL;
    attr_data[2] = &dev;
    attr_type[3] = CU_POINTER_ATTRIBUTE_CONTEXT;
    attr_data[3] = &ctx;

    result = cuPointerGetAttributes(4, attr_type, attr_data, (CUdeviceptr)address);
    is_dev = (mem_type == CU_MEMORYTYPE_DEVICE);

    return (CUDA_SUCCESS != result);
}

void
nixlLibfabricCudaCtx::cudaResetCtxPtr() {
    pthrCudaCtx_ = NULL;
    myDevId_ = -1;
}

int
nixlLibfabricCudaCtx::cudaUpdateCtxPtr(void *address, int expected_dev, bool &was_updated) {
    bool is_dev;
    CUdevice dev;
    CUcontext ctx;
    int ret;

    was_updated = false;

    if (expected_dev == -1) return -1;
    if (myDevId_ != -1 && expected_dev != myDevId_) return -1;

    ret = cudaQueryAddr(address, is_dev, dev, ctx);
    if (ret) return ret;
    if (!is_dev) return 0;
    if (dev != expected_dev) return -1;

    if (pthrCudaCtx_) {
        if (pthrCudaCtx_ != ctx) return -1;
        return 0;
    }

    pthrCudaCtx_ = ctx;
    was_updated = true;
    myDevId_ = expected_dev;

    return 0;
}

int
nixlLibfabricCudaCtx::cudaSetCtx() {
    CUresult result;
    if (NULL == pthrCudaCtx_) return 0;

    result = cuCtxSetCurrent(pthrCudaCtx_);
    return (CUDA_SUCCESS == result);
}

void
nixlLibfabricEngine::vramInitCtx() {
    cudaCtx_ = std::make_unique<nixlLibfabricCudaCtx>();
}

int
nixlLibfabricEngine::vramUpdateCtx(void *address, uint64_t devId, bool &restart_reqd) {
    int ret;
    bool was_updated;

    restart_reqd = false;

    if (!cuda_addr_wa_) {
        return 0; // Nothing to do
    }

    ret = cudaCtx_->cudaUpdateCtxPtr(address, devId, was_updated);
    if (ret) {
        return ret;
    }

    restart_reqd = was_updated;
    return 0;
}

int
nixlLibfabricEngine::vramApplyCtx() {
    if (!cuda_addr_wa_) {
        return 0; // Nothing to do
    }
    return cudaCtx_->cudaSetCtx();
}

void
nixlLibfabricEngine::vramFiniCtx() {
    cudaCtx_.reset();
}
#endif

/****************************************
 * Request Management
 *****************************************/

nixlLibfabricBackendH::nixlLibfabricBackendH() : completed_requests_(0), total_requests_used_(0) {
    NIXL_DEBUG << "constructor called, this: " << this
               << " total_requests_used=" << total_requests_used_.load();
}

nixlLibfabricBackendH::~nixlLibfabricBackendH() {
    NIXL_DEBUG << "destructor called, this: " << this;
}

// Multi-request completion tracking methods
void
nixlLibfabricBackendH::init_request_tracking(size_t num_requests) {
    total_requests_used_.store(num_requests);
    completed_requests_.store(0);
    NIXL_DEBUG << "Initialized request tracking for " << num_requests << " requests";
}

void
nixlLibfabricBackendH::increment_completed_requests() {
    size_t completed = completed_requests_.fetch_add(1);
    NIXL_DEBUG << "Request completed, total completed: " << completed << "/"
               << total_requests_used_.load();
}

size_t
nixlLibfabricBackendH::get_completed_requests_count() const {
    return completed_requests_.load();
}

size_t
nixlLibfabricBackendH::get_total_requests_used() const {
    return total_requests_used_.load();
}

void
nixlLibfabricBackendH::adjust_total_requests(size_t actual_count) {
    total_requests_used_.store(actual_count);
    NIXL_DEBUG << "Adjusted total requests to actual count: " << actual_count;
}

bool
nixlLibfabricBackendH::is_completed() const {
    // Transfer is completed when all requests have completed
    // NIXL_DEBUG << "Request completed, total completed: " << completed_requests_.load();
    return completed_requests_.load() == total_requests_used_.load();
}

/****************************************
 * Constructor/Destructor
 *****************************************/

nixlLibfabricEngine::nixlLibfabricEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params),
      cm_thread_stop_(false),
      progress_thread_enabled_(init_params->enableProgTh),
      progress_thread_delay_(std::chrono::microseconds(init_params->pthrDelay)),
      rail_manager(NIXL_LIBFABRIC_DEFAULT_STRIPING_THRESHOLD) {

    NIXL_DEBUG << "Initializing Libfabric Backend with GPU Support";

#ifdef HAVE_CUDA
    // Initialize CUDA context management
    vramInitCtx();
    // CUDA address workaround
    if (getenv("NIXL_DISABLE_CUDA_ADDR_WA")) {
        NIXL_DEBUG << "Disabling CUDA address workaround";
        cuda_addr_wa_ = false;
    } else {
        cuda_addr_wa_ = true;
        NIXL_DEBUG << "CUDA address workaround enabled";
    }
#endif

    // Parse striping threshold parameter
    std::string threshold_str;
    striping_threshold_ = NIXL_LIBFABRIC_DEFAULT_STRIPING_THRESHOLD;

    if (getInitParam("striping_threshold", threshold_str) == NIXL_SUCCESS) {
        try {
            striping_threshold_ = std::stoull(threshold_str);
            NIXL_DEBUG << "Using custom striping threshold: " << striping_threshold_ << " bytes";
        }
        catch (const std::exception &e) {
            NIXL_WARN << "Invalid striping_threshold value '" << threshold_str
                      << "', using default: " << striping_threshold_ << " bytes";
        }
    } else {
        NIXL_DEBUG << "Using default striping threshold: " << striping_threshold_ << " bytes";
    }

    // Initialize Rail Manager which will discover the topology and create all rails.
    try {
        NIXL_DEBUG << "Rail Manager created with " << rail_manager.getNumDataRails()
                   << " data rails and " << rail_manager.getNumControlRails() << " control rails";

        // Set up callbacks on each rail using Engine's static callback functions
        size_t control_rail_id = 0;
        NIXL_DEBUG << "Set notification processor for control rail 0";
        rail_manager.getControlRail(control_rail_id)
            .setNotificationCallback([this](const std::string &serialized_notif) {
                processNotification(serialized_notif);
            });

        // Set up connection state callbacks for control rails
        NIXL_DEBUG << "Set connection state processor for CM rail 0";

        rail_manager.getControlRail(control_rail_id)
            .setConnectionAckCallback([this](const uint16_t agent_idx,
                                             nixlLibfabricConnection *conn_info,
                                             ConnectionState state) {
                processConnectionAck(agent_idx, conn_info, state);
            });

        // Set up connection request callback for control rails
        rail_manager.getControlRail(control_rail_id)
            .setConnectionReqCallback([this](const uint16_t agent_idx,
                                             const std::string &serialized_data,
                                             nixlLibfabricRail *rail) -> nixl_status_t {
                return processConnectionRequest(agent_idx, serialized_data, rail);
            });

        // Set up XFER_ID tracking callbacks for all data rails
        NIXL_DEBUG << "Setting up XFER_ID tracking callbacks for " << rail_manager.getNumDataRails()
                   << " data rails";
        for (size_t data_rail_id = 0; data_rail_id < rail_manager.getNumDataRails();
             ++data_rail_id) {
            rail_manager.getDataRail(data_rail_id).setXferIdCallback([this](uint32_t xfer_id) {
                addReceivedXferId(xfer_id);
            });
            NIXL_DEBUG << "Set XFER_ID callback for data rail " << data_rail_id;
        }

        // Create self-connection
        std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> data_endpoints(
            rail_manager.getNumDataRails());
        std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> control_endpoints(
            rail_manager.getNumControlRails());
        // Prepare data rail endpoints
        for (size_t rail_id = 0; rail_id < rail_manager.getNumDataRails(); ++rail_id) {
            std::memcpy(data_endpoints[rail_id].data(),
                        rail_manager.getDataRail(rail_id).ep_name,
                        sizeof(rail_manager.getDataRail(rail_id).ep_name));
        }
        // Prepare control rail endpoints
        for (size_t rail_id = 0; rail_id < rail_manager.getNumControlRails(); ++rail_id) {
            std::memcpy(control_endpoints[rail_id].data(),
                        rail_manager.getControlRail(rail_id).ep_name,
                        sizeof(rail_manager.getControlRail(rail_id).ep_name));
        }
        // Create self-connection using common method
        nixl_status_t conn_status =
            createAgentConnection(localAgent, data_endpoints, control_endpoints);
        if (conn_status != NIXL_SUCCESS) {
            throw std::runtime_error(
                "createAgentConnection failed for self-connection with status: " +
                std::to_string(conn_status));
        }

        NIXL_DEBUG << "Created self-connection for agent: " << localAgent << " on "
                   << rail_manager.getNumDataRails() << " data rails and "
                   << rail_manager.getNumControlRails() << " control rails";

        // Threading infrastructure
        // Start CM thread for background processing
        NIXL_DEBUG << "Starting CM thread";
        cm_thread_ = std::thread(&nixlLibfabricEngine::cmThread, this);
        if (!cm_thread_.joinable()) {
            NIXL_ERROR << "Failed to start CM thread";
            throw std::runtime_error("Failed to start CM thread");
        }
        NIXL_DEBUG << "ConnectionManagement thread started successfully";

        // Start Progress thread for data rail completion processing
        if (progress_thread_enabled_) {
            NIXL_DEBUG << "Starting Progress thread for data rails with delay: "
                       << progress_thread_delay_.count() << " microseconds";
            progress_thread_stop_ = false;
            progress_thread_ = std::thread(&nixlLibfabricEngine::progressThread, this);

            if (!progress_thread_.joinable()) {
                NIXL_ERROR << "Failed to start Progress thread";
                throw std::runtime_error("Failed to start Progress thread");
            }
            NIXL_DEBUG << "Progress thread started successfully";
        } else {
            NIXL_DEBUG << "Progress thread disabled, using manual progress in checkXfer/getNotifs";
        }
    }
    catch (const std::exception &e) {
        cleanup();
        throw;
    }
}

nixlLibfabricEngine::~nixlLibfabricEngine() {
    NIXL_DEBUG
        << "Destructor starting, stopping all threads FIRST to prevent timing report interruption";

    // STOP ALL THREADS FIRST to prevent any interference with timing report
    cm_thread_stop_.store(true);

    if (progress_thread_enabled_) {
        progress_thread_stop_.store(true);
    }

    // Post dummy completion to wake up blocking threads
    postShutdownCompletion();

    if (cm_thread_.joinable()) {
        NIXL_DEBUG << "Waiting for CM thread to exit";
        cm_thread_.join();
        NIXL_DEBUG << "CM thread joined successfully";
    }
    if (progress_thread_enabled_ && progress_thread_.joinable()) {
        NIXL_DEBUG << "Waiting for Progress thread to exit";
        progress_thread_.join();
        NIXL_DEBUG << "Progress thread joined successfully";
    } else if (!progress_thread_enabled_) {
        NIXL_DEBUG << "Progress thread was not running";
    }
    NIXL_DEBUG << "All threads stopped, now cleaning up resources";
    cleanup();
}

/****************************************
 * Connection management
 *****************************************/

nixl_status_t
nixlLibfabricEngine::getConnInfo(std::string &str) const {
    // Verify all rail endpoints are initialized
    for (size_t rail_id = 0; rail_id < rail_manager.getNumDataRails(); ++rail_id) {
        if (!rail_manager.getDataRail(rail_id).endpoint) {
            NIXL_ERROR << "Rail " << rail_id << " endpoint not initialized";
            return NIXL_ERR_BACKEND;
        }
    }

    NIXL_DEBUG << "Retrieving local endpoint addresses for all " << rail_manager.getNumDataRails()
               << " rails";

    // Use Rail Manager's connection SerDes method with "dest" prefix for remote consumption
    nixl_status_t status = rail_manager.serializeConnectionInfo("dest", str);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail Manager serializeConnectionInfo failed";
        return status;
    }

    NIXL_DEBUG << "Rail Manager serialized connection info for " << rail_manager.getNumDataRails()
               << " rails, " << rail_manager.getNumControlRails() << " control rails, "
               << "total size: " << str.length();

    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::loadRemoteConnInfo(const std::string &remote_agent,
                                        const std::string &remote_conn_info) {
    std::lock_guard<std::mutex> lock(connection_state_mutex_);

    NIXL_DEBUG << "Loading remote info for agent: " << remote_agent
               << ", info length: " << remote_conn_info.length()
               << ", info (hex): " << LibfabricUtils::hexdump(remote_conn_info.data());

    if (remote_conn_info.empty()) {
        NIXL_ERROR << "Empty remote connection info received";
        return NIXL_ERR_INVALID_PARAM;
    }

    NIXL_DEBUG << "Processing " << rail_manager.getNumDataRails() << " data rails and "
               << rail_manager.getNumControlRails() << " control rails for agent: " << remote_agent;

    // Use Rail Manager's connection SerDes method with "dest" prefix (remote is sending us their
    // endpoints as "dest")
    std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> data_endpoints;
    std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> control_endpoints;
    nixl_status_t status = rail_manager.deserializeConnectionInfo(
        "dest", remote_conn_info, data_endpoints, control_endpoints);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail Manager deserializeConnectionInfo failed";
        return status;
    }
    // Create connection to remote agent
    nixl_status_t conn_status =
        createAgentConnection(remote_agent, data_endpoints, control_endpoints);
    if (conn_status != NIXL_SUCCESS) {
        NIXL_ERROR << "createAgentConnection failed with status: " << conn_status;
        return conn_status;
    }

    NIXL_DEBUG << "Successfully stored multirail connection for " << remote_agent << " on "
               << rail_manager.getNumDataRails() << " rails";
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::connect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(connection_state_mutex_);

    NIXL_DEBUG << "Connecting to agent: " << remote_agent
               << ", connections_ size: " << connections_.size();

    // Check if connection is already established
    auto it = connections_.find(remote_agent);
    if (it != connections_.end() && it->second->overall_state_ == ConnectionState::CONNECTED) {
        NIXL_DEBUG << "Connection already established for " << remote_agent
                   << ", fi_addr: " << it->second->rail_remote_addr_list_[0];
        return NIXL_SUCCESS;
    }

    // Connection exists but not established - trigger establishConnection()
    NIXL_DEBUG << "Connection exists but not established, triggering establishConnection for "
               << remote_agent;

    // Release the lock before calling establishConnection since it acquires the same mutex
    lock.~lock_guard();

    nixl_status_t status = establishConnection(remote_agent);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to establish connection with " << remote_agent;
        return status;
    }

    it = connections_.find(remote_agent);
    if (it == connections_.end()) {
        NIXL_DEBUG << "Connect failed. No metadata connection info for " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    NIXL_DEBUG << "Successfully established connection for " << remote_agent;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::disconnect(const std::string &remote_agent) {
    std::lock_guard<std::mutex> lock(connection_state_mutex_);
    auto it = connections_.find(remote_agent);
    if (it == connections_.end()) {
        NIXL_ERROR << "Disconnect failed. No metadata connection info for " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }
    // Connection exists - check if already disconnected
    if (it->second->overall_state_ == ConnectionState::DISCONNECTED) {
        NIXL_DEBUG << "Connection already established for " << remote_agent
                   << ", fi_addr: " << it->second->rail_remote_addr_list_[0];
        return NIXL_SUCCESS;
    }

    NIXL_DEBUG << "Disconnecting from agent: " << remote_agent;

    if (remote_agent != localAgent) {
        // Send disconnect control message to remote peer - fire and forget semantics
        NIXL_DEBUG << "Sending disconnect notification to remote agent: " << remote_agent;

        // Use rail manager's serialization method with "src" prefix (we are sending our source
        // endpoints)
        std::string serialized_conn_info;
        nixl_status_t serialize_status =
            rail_manager.serializeConnectionInfo("src", serialized_conn_info);
        if (serialize_status != NIXL_SUCCESS) {
            NIXL_ERROR << "Rail manager serializeConnectionInfo failed";
            return serialize_status;
        }

        // Allocate control request
        const size_t control_rail_id = 0;
        const size_t buffer_size =
            (rail_manager.getNumDataRails() + rail_manager.getNumControlRails()) *
                LF_EP_NAME_MAX_LEN +
            1024;
        nixlLibfabricReq *control_request =
            rail_manager.getControlRail(control_rail_id).allocateControlRequest(buffer_size);
        if (!control_request) {
            NIXL_ERROR << "Failed to allocate control request for disconnect";
            return NIXL_ERR_BACKEND;
        }

        memcpy(control_request->buffer, serialized_conn_info.data(), serialized_conn_info.length());

        // Set the actual size of serialized data
        control_request->buffer_size = serialized_conn_info.length();

        nixl_status_t status = rail_manager.postControlMessage(
            nixlLibfabricRailManager::ControlMessageType::DISCONNECT_REQ,
            control_request,
            it->second->control_rail_remote_addr_list_[0], // Use control rail 0
            it->second->agent_index_);

        if (status != NIXL_SUCCESS) {
            NIXL_WARN << "Failed to send disconnect notification to " << remote_agent
                      << ", proceeding with local cleanup anyway";
            // Continue with cleanup even if notification failed
        } else {
            NIXL_DEBUG << "Disconnect notification sent successfully to " << remote_agent;
        }
    } else {
        NIXL_DEBUG << "Skipping disconnect notification for self-connection";
    }

    // Clean up libfabric resources (AV entries) via rail manager
    NIXL_DEBUG << "Cleaning up libfabric resources for agent: " << remote_agent;
    // Clean up libfabric per-connection AV cleanup for both data and control rails
    nixl_status_t data_cleanup_status = rail_manager.cleanupConnection(
        nixlLibfabricRailManager::RailType::DATA, it->second->rail_remote_addr_list_);
    if (data_cleanup_status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to clean up data rail resources for agent: " << remote_agent
                   << " with status: " << data_cleanup_status;
        return data_cleanup_status;
    }

    nixl_status_t control_cleanup_status = rail_manager.cleanupConnection(
        nixlLibfabricRailManager::RailType::CONTROL, it->second->control_rail_remote_addr_list_);
    if (control_cleanup_status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to clean up control rail resources for agent: " << remote_agent
                   << " with status: " << control_cleanup_status;
        return control_cleanup_status;
    }

    NIXL_DEBUG << "Successfully cleaned up libfabric resources for agent: " << remote_agent;

    // Update connection state to DISCONNECTED before removing
    it->second->overall_state_ = ConnectionState::DISCONNECTED;

    // Remove connection from map
    connections_.erase(remote_agent);
    NIXL_DEBUG << "Connection erased from the connection map for agent: " << remote_agent;

    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::createAgentConnection(
    const std::string &agent_name,
    const std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> &data_rail_endpoints,
    const std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> &control_rail_endpoints) {

    NIXL_DEBUG << "Creating connection for agent: " << agent_name;

    // Validate input parameters
    if (data_rail_endpoints.size() != rail_manager.getNumDataRails()) {
        NIXL_ERROR << "Expected " << rail_manager.getNumDataRails() << " data rail endpoints, got "
                   << data_rail_endpoints.size();
        return NIXL_ERR_INVALID_PARAM;
    }

    if (control_rail_endpoints.size() != rail_manager.getNumControlRails()) {
        NIXL_ERROR << "Expected " << rail_manager.getNumControlRails()
                   << " control rail endpoints, got " << control_rail_endpoints.size();
        return NIXL_ERR_INVALID_PARAM;
    }

    // Create connection object
    auto conn = std::make_shared<nixlLibfabricConnection>();
    if (!conn) {
        NIXL_ERROR << "Failed to allocate connection object";
        return NIXL_ERR_BACKEND;
    }

    conn->remoteAgent_ = agent_name;
    conn->rail_remote_addr_list_.reserve(rail_manager.getNumDataRails());
    conn->control_rail_remote_addr_list_.reserve(rail_manager.getNumControlRails());

    // Process all data rails in one operation
    nixl_status_t data_status =
        rail_manager.insertAllAddresses(nixlLibfabricRailManager::RailType::DATA,
                                        data_rail_endpoints,
                                        conn->rail_remote_addr_list_,
                                        conn->src_ep_names_);
    if (data_status != NIXL_SUCCESS) {
        NIXL_ERROR << "insertAllAddresses failed for data rails with status: " << data_status;
        return NIXL_ERR_BACKEND;
    }

    // Process all control rails in one operation
    nixl_status_t control_status =
        rail_manager.insertAllAddresses(nixlLibfabricRailManager::RailType::CONTROL,
                                        control_rail_endpoints,
                                        conn->control_rail_remote_addr_list_,
                                        conn->control_ep_names_);
    if (control_status != NIXL_SUCCESS) {
        NIXL_ERROR << "insertAllAddresses failed for control rails with status: " << control_status;
        return NIXL_ERR_BACKEND;
    }

    // Manage agent names and index
    agent_names_.push_back(agent_name);
    int index = 0;
    std::for_each(agent_names_.begin(), agent_names_.end(), [&index](const std::string &name) {
        NIXL_DEBUG << "Index " << index << ": " << name;
        index++;
    });
    conn->agent_index_ = agent_names_.size() - 1;

    // Store connection
    connections_[agent_name] = conn;

    NIXL_DEBUG << "Successfully created connection for agent: " << agent_name << " on "
               << rail_manager.getNumDataRails() << " data rails and "
               << rail_manager.getNumControlRails() << " control rails";

    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::establishConnection(const std::string &remote_agent) const {
    // Use existing connection_state_mutex_ to serialize connection establishment
    std::lock_guard<std::mutex> lock(connection_state_mutex_);

    // Check if another thread already established the connection
    auto it = connections_.find(remote_agent);
    if (it != connections_.end() && it->second->overall_state_ == ConnectionState::CONNECTED) {
        NIXL_DEBUG << "Connection already established by another thread for " << remote_agent;
        return NIXL_SUCCESS;
    }

    if (it == connections_.end()) {
        NIXL_ERROR << "No connection found for agent: " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }


    // Verify we have addresses for all data rails
    if (it->second->rail_remote_addr_list_.size() != rail_manager.getNumDataRails()) {
        NIXL_ERROR << "Remote connection has " << it->second->rail_remote_addr_list_.size()
                   << " data rails, expected " << rail_manager.getNumDataRails();
        return NIXL_ERR_BACKEND;
    }

    NIXL_DEBUG << "Establishing connections_ on control rails and data rails for agent: "
               << remote_agent;

    // Use single "Communicator" for CM
    auto *conn_info = reinterpret_cast<nixlLibfabricConnection *>(it->second.get());

    NIXL_DEBUG << "Using connection info : 0: "
               << LibfabricUtils::hexdump(conn_info->src_ep_names_[0]) << std::endl
               << "1: " << LibfabricUtils::hexdump(conn_info->src_ep_names_[1]) << std::endl
               << "control_0: " << LibfabricUtils::hexdump(conn_info->control_ep_names_[0])
               << std::endl
               << " with agent index: " << it->second->agent_index_;
    if (!conn_info) {
        NIXL_ERROR << "Connection info for agent " << remote_agent << " is null";
        return NIXL_ERR_BACKEND;
    }

    // Allocate control request
    const size_t control_rail_id = 0;

    // Serialize connection info
    std::string serialized_conn_info;
    nixl_status_t serialize_status =
        rail_manager.serializeConnectionInfo("src", serialized_conn_info);
    if (serialize_status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail manager serializeConnectionInfo failed";
        return serialize_status;
    }

    nixlLibfabricReq *control_request = rail_manager.getControlRail(control_rail_id)
                                            .allocateControlRequest(serialized_conn_info.length());
    if (!control_request) {
        NIXL_ERROR << "Failed to allocate control request for connection establishment";
        return NIXL_ERR_BACKEND;
    }

    // Copy serialized data to control request buffer
    memcpy(control_request->buffer, serialized_conn_info.data(), serialized_conn_info.length());
    control_request->buffer_size = serialized_conn_info.length();

    nixl_status_t status = rail_manager.postControlMessage(
        nixlLibfabricRailManager::ControlMessageType::CONNECTION_REQ,
        control_request,
        conn_info->control_rail_remote_addr_list_[0], // Always use control rail 0
        it->second->agent_index_ // agent_index is only used in the ACK back from remote,
                                 // to match connection request
    );
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "postSend failed on rail " << 0;
        // TODO, wrap req info into a nixlLibfabricRequestHandle and add retry logic
        return NIXL_ERR_BACKEND;
    }
    // Register the connection state tracker with the CM thread
    // Wait for the CM thread to establish the connection
    // TODO: Currently blocking, update to timeout and return NIXL_IN_PROG
    {
        std::unique_lock<std::mutex> lock(conn_info->conn_state_mutex_);
        NIXL_DEBUG << "Waiting for connection to be established for agent: " << remote_agent;
        conn_info->cv_.wait(lock, [conn_info] {
            return conn_info->overall_state_ == ConnectionState::CONNECTED ||
                conn_info->overall_state_ == ConnectionState::FAILED;
        });
        NIXL_DEBUG << "Connection state for agent " << remote_agent << " is now "
                   << conn_info->overall_state_;

        if (conn_info->overall_state_ == ConnectionState::FAILED) {
            NIXL_ERROR << "Connection failed on control rail 0";
            return NIXL_ERR_BACKEND;
        }
    }

    NIXL_DEBUG << "Connection already established for agent: " << remote_agent;
    return NIXL_SUCCESS;
}

/****************************************
 * Memory management
 *****************************************/

nixl_mem_list_t
nixlLibfabricEngine::getSupportedMems() const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
#ifdef HAVE_CUDA
    mems.push_back(VRAM_SEG);
#endif
    return mems;
}

nixl_status_t
nixlLibfabricEngine::registerMem(const nixlBlobDesc &mem,
                                 const nixl_mem_t &nixl_mem,
                                 nixlBackendMD *&out) {
    auto priv = std::make_unique<nixlLibfabricPrivateMetadata>();

    priv->buffer_ = (void *)mem.addr;
    priv->length_ = mem.len;
    priv->gpu_device_id_ = mem.devId; // Store GPU device ID

#ifdef HAVE_CUDA
    // Handle CUDA memory registration with GPU Direct RDMA support
    if (nixl_mem == VRAM_SEG) {
        // For multi-GPU support, skip CUDA address workaround
        if (cuda_addr_wa_) {
            bool need_restart;
            if (vramUpdateCtx((void *)mem.addr, mem.devId, need_restart)) {
                NIXL_WARN << "CUDA address workaround failed for device " << mem.devId
                          << ", disabling workaround for multi-GPU support";
                cuda_addr_wa_ = false; // Disable workaround for subsequent registrations
            } else if (need_restart) {
                // Restart progress thread if needed
                NIXL_DEBUG << "CUDA context updated, restarting progress thread";
                vramApplyCtx();
            }
        }
        // Set CUDA device context directly for multi-GPU support
        if (!cuda_addr_wa_) {
            cudaError_t cuda_ret = cudaSetDevice(mem.devId);
            if (cuda_ret != cudaSuccess) {
                NIXL_ERROR << "Failed to set CUDA device " << mem.devId << ": "
                           << cudaGetErrorString(cuda_ret);
                return NIXL_ERR_NOT_SUPPORTED;
            }
            NIXL_DEBUG << "Set CUDA device context to GPU " << mem.devId;
        }
    }
#endif

    // Initialize vectors to accommodate all possible rails (for indexing consistency)
    priv->rail_mr_list_.resize(rail_manager.getNumDataRails(), nullptr);
    priv->rail_key_list_.resize(rail_manager.getNumDataRails(), 0);

#ifdef HAVE_CUDA
    // Set CUDA context before libfabric operations for VRAM
    if (nixl_mem == VRAM_SEG) {
        vramApplyCtx();
    }
#endif

    // Use Rail Manager for centralized memory registration with GPU Direct RDMA support
    nixl_status_t status = rail_manager.registerMemory((void *)mem.addr,
                                                       mem.len,
                                                       nixl_mem,
                                                       priv->rail_mr_list_,
                                                       priv->rail_key_list_,
                                                       priv->selected_rails_);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail Manager registerMemory failed";
        return status;
    }

    NIXL_DEBUG << "Rail Manager successfully registered "
               << (nixl_mem == VRAM_SEG ? "VRAM" : "DRAM") << " memory on "
               << priv->selected_rails_.size() << " rails"
               << (nixl_mem == VRAM_SEG ? " with GPU Direct RDMA support" : "");

    NIXL_DEBUG << "Successfully registered memory on " << priv->selected_rails_.size()
               << " rails for " << (nixl_mem == VRAM_SEG ? "GPU" : "CPU") << " " << mem.devId;
    out = priv.release();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::deregisterMem(nixlBackendMD *meta) {
    auto *priv = static_cast<nixlLibfabricPrivateMetadata *>(meta);
    // Use Rail Manager for centralized memory deregistration
    nixl_status_t status =
        rail_manager.deregisterMemory(priv->selected_rails_, priv->rail_mr_list_);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail Manager deregisterMemory failed";
        // Continue with cleanup even if deregistration failed
    }

    delete priv;
    return status;
}

nixl_status_t
nixlLibfabricEngine::getPublicData(const nixlBackendMD *meta, std::string &str) const {
    const nixlLibfabricPrivateMetadata *priv =
        static_cast<const nixlLibfabricPrivateMetadata *>(meta);

    return rail_manager.serializeMemoryKeys(priv->rail_key_list_, priv->buffer_, str);
}

nixl_status_t
nixlLibfabricEngine::loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) {
    nixlLibfabricPrivateMetadata *input_md = static_cast<nixlLibfabricPrivateMetadata *>(input);
    auto pub_md = std::make_unique<nixlLibfabricPublicMetadata>();
    // Store all rail keys instead of just the first one
    pub_md->rail_remote_key_list_.reserve(input_md->rail_key_list_.size());
    for (size_t rail_id = 0; rail_id < input_md->rail_key_list_.size(); ++rail_id) {
        pub_md->rail_remote_key_list_.push_back(input_md->rail_key_list_[rail_id]);
        NIXL_DEBUG << "Added rail " << rail_id << " key: " << input_md->rail_key_list_[rail_id];
    }

    pub_md->remote_buf_addr_ = reinterpret_cast<uint64_t>(input_md->buffer_);
    pub_md->conn_ = connections_[localAgent];

    output = pub_md.release();
    NIXL_DEBUG << "Loading Local MD with " << input_md->rail_key_list_.size() << " rail keys";
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::loadRemoteMD(const nixlBlobDesc &input,
                                  const nixl_mem_t &nixl_mem,
                                  const std::string &remote_agent,
                                  nixlBackendMD *&output) {
    NIXL_DEBUG << "Loading remote metadata for agent: " << remote_agent;

    auto conn_it = connections_.find(remote_agent);
    if (conn_it == connections_.end()) {
        NIXL_ERROR << "Could not find connection for agent: " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    // Delegate to Rail Manager for SerDes operations (returns raw data)
    std::vector<uint64_t> remote_keys;
    uint64_t remote_addr;
    nixl_status_t status =
        rail_manager.deserializeMemoryKeys(input.metaInfo, remote_keys, remote_addr);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Rail Manager deserializeMemoryKeys failed";
        return status;
    }

    // Engine handles connection management and metadata object creation
    auto pub_md = std::make_unique<nixlLibfabricPublicMetadata>();
    pub_md->conn_ = conn_it->second;
    pub_md->rail_remote_key_list_ = std::move(remote_keys);
    pub_md->remote_buf_addr_ = remote_addr;
    NIXL_DEBUG << "Remote metadata loaded with"
               << " Remote addr: " << (void *)pub_md->remote_buf_addr_ << " Remote keys for "
               << pub_md->rail_remote_key_list_.size() << " rails"
               << " Remote fi_addr: " << pub_md->conn_->rail_remote_addr_list_[0];

    output = pub_md.release();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::unloadMD(nixlBackendMD *input) {
    delete input;
    return NIXL_SUCCESS;
}

/****************************************
 * Data movement
 *****************************************/

nixl_status_t
nixlLibfabricEngine::prepXfer(const nixl_xfer_op_t &operation,
                              const nixl_meta_dlist_t &local,
                              const nixl_meta_dlist_t &remote,
                              const std::string &remote_agent,
                              nixlBackendReqH *&handle,
                              const nixl_opt_b_args_t *opt_args) const {
    NIXL_DEBUG << "Preparing transfer for remote_agent: " << remote_agent;

    auto conn_it = connections_.find(remote_agent);
    if (conn_it == connections_.end() || !conn_it->second) {
        NIXL_ERROR << "No valid connection found for agent: " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    auto backend_handle = new nixlLibfabricBackendH();
    if (!backend_handle) {
        NIXL_ERROR << "Failed to allocate nixlLibfabricBackendH";
        return NIXL_ERR_BACKEND;
    }
    handle = backend_handle; // Assign to base class pointer

    NIXL_DEBUG << "Transfer preparation complete, handle address: " << handle;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::estimateXferCost(const nixl_xfer_op_t &operation,
                                      const nixl_meta_dlist_t &local,
                                      const nixl_meta_dlist_t &remote,
                                      const std::string &remote_agent,
                                      nixlBackendReqH *const &handle,
                                      std::chrono::microseconds &duration,
                                      std::chrono::microseconds &err_margin,
                                      nixl_cost_t &method,
                                      const nixl_opt_args_t *opt_args) const {
    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::postXfer(const nixl_xfer_op_t &operation,
                              const nixl_meta_dlist_t &local,
                              const nixl_meta_dlist_t &remote,
                              const std::string &remote_agent,
                              nixlBackendReqH *&handle,
                              const nixl_opt_b_args_t *opt_args) const {

    // Validate connection
    auto conn_it = connections_.find(remote_agent);
    if (conn_it == connections_.end() || !conn_it->second) {
        NIXL_ERROR << "No valid connection found for agent: " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    if (conn_it->second->overall_state_ == ConnectionState::DISCONNECTED) {
        NIXL_DEBUG << "No existing connection for " << remote_agent
                   << ", establishing new connection";
        nixl_status_t status = this->establishConnection(remote_agent);
        if (status != NIXL_SUCCESS) {
            NIXL_ERROR << "Failed to establish connection with " << remote_agent;
            return status;
        }
        NIXL_DEBUG << "Established new connection with remote_agent: " << remote_agent;
    }

    NIXL_DEBUG << "Posting transfer for remote_agent: " << remote_agent
               << ", handle address: " << handle;

    auto backend_handle = static_cast<nixlLibfabricBackendH *>(handle);
    if (!backend_handle) {
        NIXL_ERROR << "Failed to cast handle to nixlLibfabricBackendH";
        return NIXL_ERR_INVALID_PARAM;
    }

    // Allocate a new notification request at the start of each postXfer
    const size_t control_rail_id = 0;
    nixlLibfabricReq *control_request = rail_manager.getControlRail(control_rail_id)
                                            .allocateControlRequest(sizeof(BinaryNotification));
    if (!control_request) {
        NIXL_ERROR << "Failed to allocate control request for notification";
        return NIXL_ERR_BACKEND;
    }

    // Create BinaryNotification directly in the control request buffer
    BinaryNotification *binary_notif =
        reinterpret_cast<BinaryNotification *>(control_request->buffer);
    binary_notif->clear();

    nixlLibfabricReq::OpType op_type;
    int desc_count = local.descCount();

    NIXL_DEBUG << "Processing " << desc_count
               << " descriptors using optimized single-pass approach";

    op_type = (operation == NIXL_WRITE) ? nixlLibfabricReq::WRITE : nixlLibfabricReq::READ;

    // Set initial request count to maximum possible requests
    size_t max_possible_requests = desc_count * rail_manager.getNumDataRails();
    backend_handle->init_request_tracking(max_possible_requests);

    // Core transfer submission to process each descriptor with direct submission
    for (int desc_idx = 0; desc_idx < desc_count; ++desc_idx) {
        auto *local_md = static_cast<nixlLibfabricPrivateMetadata *>(local[desc_idx].metadataP);
        auto *remote_md = static_cast<nixlLibfabricPublicMetadata *>(remote[desc_idx].metadataP);
        if (!local_md || !remote_md || !remote_md->conn_) {
            NIXL_ERROR << "Invalid metadata pointers for descriptor " << desc_idx;
            return NIXL_ERR_INVALID_PARAM;
        }

        // Validate connection for this descriptor
        if (remote_md->conn_ != conn_it->second) {
            NIXL_ERROR << "Connection mismatch for descriptor " << desc_idx;
            return NIXL_ERR_MISMATCH;
        }
        // Get transfer info for THIS descriptor
        void *transfer_addr = (void *)local[desc_idx].addr;
        size_t transfer_size = local[desc_idx].len;
        int gpu_id = local[desc_idx].devId;

        NIXL_DEBUG << "Processing descriptor " << desc_idx << " GPU " << gpu_id
                   << " addr: " << transfer_addr << " size: " << transfer_size;

        // Prepare and submit transfer
        nixl_status_t status = rail_manager.prepareAndSubmitTransfer(
            op_type,
            transfer_addr,
            transfer_size,
            remote_md->remote_buf_addr_,
            local_md->selected_rails_,
            local_md->rail_mr_list_,
            remote_md->rail_remote_key_list_,
            conn_it->second->rail_remote_addr_list_,
            conn_it->second->agent_index_,
            [backend_handle]() {
                backend_handle->increment_completed_requests();
            }, // Completion callback
            binary_notif // Populate BinaryNotification
        );

        if (status != NIXL_SUCCESS) {
            NIXL_ERROR << "prepareAndSubmitTransfer failed for descriptor " << desc_idx << " GPU "
                       << gpu_id;
            return status;
        }

        NIXL_DEBUG << "Successfully processed descriptor " << desc_idx << " with "
                   << binary_notif->xfer_id_count << " requests submitted";
    }

    NIXL_DEBUG << "Processing complete: submitted " << binary_notif->xfer_id_count
               << " requests from " << desc_count << " descriptors" << " with "
               << binary_notif->xfer_id_count << " total XFER_IDs";

    // Adjust to actual request count after all submissions complete
    backend_handle->adjust_total_requests(binary_notif->xfer_id_count);

    // Send notification immediately after successful request submission
    if (opt_args && opt_args->hasNotif) {
        NIXL_DEBUG << "Sending immediate notification after successful request submission";

        // Set agent name and message in the BinaryNotification
        binary_notif->setAgentName(localAgent);
        binary_notif->setMessage(opt_args->notifMsg);

        nixl_status_t notif_status = notifSendPriv(remote_agent, control_request);
        if (notif_status != NIXL_SUCCESS) {
            NIXL_ERROR << "Failed to send immediate notification";
            return notif_status;
        }
        NIXL_DEBUG << "Immediate notification sent successfully with "
                   << binary_notif->xfer_id_count << " XFER_IDs";
    }

    // Progress data rails to kick off transfers
    if (!progress_thread_enabled_) {
        nixl_status_t progress_status = rail_manager.progressActiveDataRails();
        if (progress_status == NIXL_IN_PROG) {
            return NIXL_IN_PROG;
        }
    }

    // For very small transfers we can check for local completions immediately.
    if (backend_handle->is_completed()) {
        return NIXL_SUCCESS;
    }

    return NIXL_IN_PROG;
}

nixl_status_t
nixlLibfabricEngine::checkXfer(nixlBackendReqH *handle) const {
    auto backend_handle = static_cast<nixlLibfabricBackendH *>(handle);

    if (!progress_thread_enabled_) {
        nixl_status_t progress_status = rail_manager.progressActiveDataRails();
        if (progress_status != NIXL_SUCCESS && progress_status != NIXL_IN_PROG) {
            NIXL_ERROR << "Failed to progress data rails in checkXfer";
            return progress_status;
        }
    }
    // Then check for completions after processing any pending completions
    if (backend_handle->is_completed()) {
        NIXL_DEBUG << "Data transfer completed successfully";
        return NIXL_SUCCESS;
    }
    return NIXL_IN_PROG;
}

nixl_status_t
nixlLibfabricEngine::releaseReqH(nixlBackendReqH *handle) const {
    // Add any necessary cleanup for libfabric specific request handling
    // For example, if we're using a custom request structure:
    // nixlLibfabricReqH* req = static_cast<nixlLibfabricReqH*>(handle);
    // // Perform any necessary cleanup
    // delete req;

    if (!handle) {
        return NIXL_SUCCESS;
    }

    // Let NIXL framework handle the deletion
    NIXL_DEBUG << "releaseReqH completed successfully";
    return NIXL_SUCCESS;
}

// notifSendPriv that accept control request
nixl_status_t
nixlLibfabricEngine::notifSendPriv(const std::string &remote_agent,
                                   nixlLibfabricReq *control_request) const {
    auto it = connections_.find(remote_agent);
    if (it == connections_.end()) {
        NIXL_ERROR << "No connection found for agent: " << remote_agent;
        return NIXL_ERR_NOT_FOUND;
    }

    auto connection = it->second;
    const size_t control_rail_id = 0; // Only use control rail 0 for notifications

    // Set the correct buffer size for the notification
    control_request->buffer_size = sizeof(BinaryNotification);

    // Get BinaryNotification from control request buffer for logging
    BinaryNotification *binary_notif =
        reinterpret_cast<BinaryNotification *>(control_request->buffer);

    NIXL_DEBUG << "Sending binary notification control request"
               << " Message: " << binary_notif->getMessage()
               << " xfer_id_count: " << binary_notif->xfer_id_count;
    nixl_status_t status =
        rail_manager.postControlMessage(nixlLibfabricRailManager::ControlMessageType::NOTIFICATION,
                                        control_request,
                                        connection->control_rail_remote_addr_list_[control_rail_id],
                                        connection->agent_index_);

    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "postControlMessage failed on control rail " << control_rail_id;
        return NIXL_ERR_BACKEND;
    }

    return NIXL_SUCCESS;
}

nixl_status_t
nixlLibfabricEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    // For regular notifications, we need to allocate a temporary control request
    // since we don't have a pre-allocated one from prepXfer
    const size_t control_rail_id = 0;
    nixlLibfabricReq *control_req = rail_manager.getControlRail(control_rail_id)
                                        .allocateControlRequest(sizeof(BinaryNotification));
    if (!control_req) {
        NIXL_ERROR << "Failed to allocate temporary control request for genNotif";
        return NIXL_ERR_BACKEND;
    }

    // Create BinaryNotification directly in the control buffer
    BinaryNotification *binary_notif = reinterpret_cast<BinaryNotification *>(control_req->buffer);
    binary_notif->clear();
    binary_notif->setAgentName(localAgent);
    binary_notif->setMessage(msg);

    return notifSendPriv(remote_agent, control_req);
}

nixl_status_t
nixlLibfabricEngine::getNotifs(notif_list_t &notif_list) {
    if (!progress_thread_enabled_) {
        nixl_status_t progress_status = rail_manager.progressActiveDataRails();
        if (progress_status != NIXL_SUCCESS && progress_status != NIXL_IN_PROG) {
            NIXL_ERROR << "Failed to progress data rails in getNotifs";
            return progress_status;
        }
    }

    // Then check for available notifications after processing completions
    // Thread-safe access to internal notification list
    {
        std::lock_guard<std::mutex> lock(notif_mutex_);

        // Move all notifications from internal list to user's list
        notif_list.insert(notif_list.end(), notifMainList_.begin(), notifMainList_.end());

        if (!notifMainList_.empty()) {
            NIXL_DEBUG << "Retrieved " << notifMainList_.size() << " notifications";
            // Clear the internal list after copying
            notifMainList_.clear();
            return NIXL_SUCCESS;
        }

        // Clear the internal list after copying (even if empty)
        notifMainList_.clear();
    }

    return NIXL_IN_PROG;
}

/****************************************
 * ConnectionManagement Thread Function
 *****************************************/

// Background progress function that continuously processes completions on all rails
nixl_status_t
nixlLibfabricEngine::cmThread() {
    NIXL_DEBUG << "ConnectionManagement thread started successfully";
    NIXL_DEBUG << "Initial receives already posted in main thread, entering progress loop";

    // Main progress loop - continuously process completions on all rails
    while (!cm_thread_stop_.load()) {

        nixl_status_t status = rail_manager.progressAllControlRails();
        if (status == NIXL_SUCCESS) {
            NIXL_DEBUG << "Processed completions on control rails";
        } else if (status != NIXL_IN_PROG && status != NIXL_SUCCESS) {
            NIXL_ERROR << "Failed to process completions on control rails";
            return NIXL_ERR_BACKEND;
        }
        // Sleep briefly to avoid spinning too aggressively when blocking cq read is not used
        if (!rail_manager.getControlRail(0).blocking_cq_sread_supported) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(10));
        }
    }
    NIXL_DEBUG << "ConnectionManagement thread exiting cleanly";
    return NIXL_SUCCESS;
}

/****************************************
 * Progress Thread Function (Data Rails Only)
 *****************************************/

// Progress thread that continuously processes completions only on data rails
nixl_status_t
nixlLibfabricEngine::progressThread() {
    NIXL_DEBUG << "Progress thread started successfully for data rails only";
    // Main progress loop - continuously process completions only on data rails
    while (!progress_thread_stop_.load()) {
        // Process completions only on data rails (non-blocking)
        bool any_completions = false;
        nixl_status_t status = rail_manager.progressActiveDataRails();
        if (status == NIXL_SUCCESS) {
            any_completions = true;
            NIXL_DEBUG << "Processed completions on data rails";
        } else if (status != NIXL_IN_PROG && status != NIXL_SUCCESS) {
            NIXL_ERROR << "Failed to process completions on data rails";
            // Don't return error, continue for robustness
        }
        if (!any_completions) {
            std::this_thread::sleep_for(progress_thread_delay_);
        }
    }
    NIXL_DEBUG << "Progress thread exiting cleanly";
    return NIXL_SUCCESS;
}

void
nixlLibfabricEngine::postShutdownCompletion() {
    NIXL_DEBUG << "Posting shutdown signal to wake up background thread";
    // Send shutdown message to self on rail 0 if self-connection exists
    auto self_conn_it = connections_.find(localAgent);
    if (self_conn_it != connections_.end() && self_conn_it->second &&
        rail_manager.getNumDataRails() > 0) {
        const size_t rail_id = 0; // Use rail 0 for shutdown signal

        // Allocate control request
        const size_t control_rail_id = 0;
        const size_t shutdown_msg_len = 8; // "SHUTDOWN" length
        nixlLibfabricReq *control_request =
            rail_manager.getControlRail(control_rail_id).allocateControlRequest(shutdown_msg_len);
        if (!control_request) {
            NIXL_ERROR << "Failed to allocate control request for shutdown";
            return;
        }

        // Copy shutdown message to the control request buffer
        std::strcpy(static_cast<char *>(control_request->buffer), "SHUTDOWN");
        control_request->buffer_size = shutdown_msg_len;

        nixl_status_t status = rail_manager.postControlMessage(
            nixlLibfabricRailManager::ControlMessageType::DISCONNECT_REQ,
            control_request,
            self_conn_it->second->rail_remote_addr_list_[rail_id],
            self_conn_it->second->agent_index_);

        if (status == NIXL_SUCCESS) {
            NIXL_DEBUG << "Shutdown signal posted successfully on rail " << rail_id;
        } else {
            NIXL_ERROR << "Failed to post shutdown signal on rail " << rail_id;
        }
    } else {
        NIXL_ERROR << "Could not find self-connection or rails not initialized";
    }
}

/****************************************
 * Static Callback Functions
 *****************************************/

void
nixlLibfabricEngine::processNotification(const std::string &serialized_notif) {
    // Only handle binary notification format
    // Check if this is a binary notification (fixed size)
    NIXL_DEBUG << "Received notification size: " << serialized_notif.size()
               << ", sizeof(BinaryNotification): " << sizeof(BinaryNotification);

    if (serialized_notif.size() != sizeof(BinaryNotification)) {
        NIXL_ERROR << "Invalid notification size: " << serialized_notif.size()
                   << ", expected: " << sizeof(BinaryNotification);
        return;
    }

    // Process binary notification format
    const BinaryNotification *binary_notif =
        reinterpret_cast<const BinaryNotification *>(serialized_notif.data());

    std::string remote_name = binary_notif->getAgentName();
    std::string msg = binary_notif->getMessage();
    std::unordered_set<uint32_t> expected_xfer_ids = binary_notif->getXferIds();

    NIXL_TRACE << "Received binary notification from " << remote_name << " msg: " << msg
               << " xfer_id_count: " << binary_notif->xfer_id_count;

    // Check if this is a transfer notification that needs queuing
    if (!expected_xfer_ids.empty()) {
        std::stringstream xfer_ids_log;
        xfer_ids_log << "Expected XFER_IDs from binary notification: [";
        bool first = true;
        for (uint32_t xfer_id : expected_xfer_ids) {
            if (!first) xfer_ids_log << ", ";
            xfer_ids_log << xfer_id;
            first = false;
        }
        xfer_ids_log << "] (total: " << expected_xfer_ids.size() << ")";
        NIXL_TRACE << xfer_ids_log.str();
        // Check if all expected XFER_IDs have already arrived
        if (allXferIdsReceived(expected_xfer_ids)) {
            NIXL_TRACE
                << "All XFER_IDs already received, processing binary notification immediately";
            std::lock_guard<std::mutex> lock(notif_mutex_);
            notifMainList_.push_back({remote_name, msg});
            NIXL_DEBUG << "Binary notification processed immediately: " << msg;
        } else {
            NIXL_TRACE << "Not all XFER_IDs received yet, queuing binary notification";
            std::lock_guard<std::mutex> lock(receiver_tracking_mutex_);
            pending_notifications_.emplace_back(remote_name, msg, expected_xfer_ids);
            NIXL_TRACE << "Binary notification queued for later processing: " << msg;
        }
    } else {
        // Regular notification without XFER_IDs - process immediately
        NIXL_TRACE << "Regular binary notification (no XFER_IDs), processing immediately";
        std::lock_guard<std::mutex> lock(notif_mutex_);
        notifMainList_.push_back({remote_name, msg});
        NIXL_TRACE << "Regular binary notification processed immediately: " << msg;
    }
}

void
nixlLibfabricEngine::processConnectionAck(uint16_t agent_idx,
                                          nixlLibfabricConnection *conn_info,
                                          ConnectionState state) {
    std::string remote_agent_name = agent_names_[agent_idx];
    NIXL_DEBUG << "Connection state callback for agent " << remote_agent_name
               << " agent_idx: " << agent_idx;
    std::lock_guard<std::mutex> lock(connections_[remote_agent_name]->conn_state_mutex_);
    connections_[remote_agent_name]->overall_state_ = ConnectionState::CONNECTED;
    connections_[remote_agent_name]->cv_.notify_all();
    NIXL_DEBUG << "Connection state updated to CONNECTED";
}

nixl_status_t
nixlLibfabricEngine::processConnectionRequest(uint16_t agent_idx,
                                              const std::string &serialized_data,
                                              nixlLibfabricRail *rail) {
    NIXL_DEBUG << "Processing connection request from agent " << agent_idx << " on rail "
               << rail->rail_id;

    // Use rail manager to deserialize ALL endpoints at once with "src" prefix (connection request
    // contains source endpoints)
    std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> data_endpoints;
    std::vector<std::array<char, LF_EP_NAME_MAX_LEN>> control_endpoints;
    nixl_status_t status = rail_manager.deserializeConnectionInfo(
        "src", serialized_data, data_endpoints, control_endpoints);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to deserialize connection info";
        return status;
    }

    // Insert ALL data rail addresses at once
    std::vector<fi_addr_t> data_fi_addrs;
    std::vector<char *> data_ep_names;
    status = rail_manager.insertAllAddresses(
        nixlLibfabricRailManager::RailType::DATA, data_endpoints, data_fi_addrs, data_ep_names);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to insert data rail addresses";
        return status;
    }

    // Insert ALL control rail addresses at once
    std::vector<fi_addr_t> control_fi_addrs;
    std::vector<char *> control_ep_names;
    status = rail_manager.insertAllAddresses(nixlLibfabricRailManager::RailType::CONTROL,
                                             control_endpoints,
                                             control_fi_addrs,
                                             control_ep_names);
    if (status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to insert control rail addresses";
        return status;
    }

    // Use the first control rail's fi_addr for ACK (same as before)
    fi_addr_t initiator_control_fi_addr = control_fi_addrs[0];

    NIXL_DEBUG << "Successfully inserted addresses for " << data_fi_addrs.size()
               << " data rails and " << control_fi_addrs.size() << " control rails"
               << ", initiator_control_fi_addr: " << initiator_control_fi_addr;

    // Send acknowledgement back to the initiator using the rail manager
    size_t ep_name_len = sizeof(rail->ep_name);

    // Allocate control request
    const size_t control_rail_id = 0;
    nixlLibfabricReq *control_request =
        rail_manager.getControlRail(control_rail_id).allocateControlRequest(ep_name_len);
    if (!control_request) {
        NIXL_ERROR << "Failed to allocate control request for connection ACK";
        return NIXL_ERR_BACKEND;
    }

    // Copy endpoint name to control request buffer
    std::memcpy(control_request->buffer, rail->ep_name, ep_name_len);
    control_request->buffer_size = ep_name_len;

    nixl_status_t ack_status = rail_manager.postControlMessage(
        nixlLibfabricRailManager::ControlMessageType::CONNECTION_ACK,
        control_request,
        initiator_control_fi_addr,
        agent_idx);
    if (ack_status != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to send ACK via rail manager";
        return ack_status;
    }

    NIXL_DEBUG << "ACK sent successfully via rail manager";
    return NIXL_SUCCESS;
}

/****************************************
 * Receiver Side XFER_ID Tracking Helper Methods
 *****************************************/

void
nixlLibfabricEngine::addReceivedXferId(uint32_t xfer_id) {
    {
        std::lock_guard<std::mutex> lock(receiver_tracking_mutex_);
        received_remote_writes_.insert(xfer_id);
        NIXL_DEBUG << "Added received XFER_ID " << xfer_id
                   << " to global tracking set (total: " << received_remote_writes_.size() << ")";
    }

    // Check if any pending notifications can now be processed
    checkPendingNotifications();
}

bool
nixlLibfabricEngine::allXferIdsReceived(const std::unordered_set<uint32_t> &expected) {
    std::lock_guard<std::mutex> lock(receiver_tracking_mutex_);
    // Check if all expected XFER_IDs are in the received set
    for (uint32_t xfer_id : expected) {
        if (received_remote_writes_.find(xfer_id) == received_remote_writes_.end()) {
            NIXL_TRACE << "XFER_ID " << xfer_id << " not yet received";
            return false;
        }
    }
    NIXL_DEBUG << "All " << expected.size() << " expected XFER_IDs have been received";
    return true;
}

/****************************************
 * Notification Queuing Helper Methods
 *****************************************/

void
nixlLibfabricEngine::checkPendingNotifications() {
    std::lock_guard<std::mutex> lock(receiver_tracking_mutex_);
    auto it = pending_notifications_.begin();
    while (it != pending_notifications_.end()) {
        // Check if all expected XFER_IDs for this notification have arrived
        bool all_received = true;
        for (uint32_t xfer_id : it->expected_xfer_ids) {
            if (received_remote_writes_.find(xfer_id) == received_remote_writes_.end()) {
                all_received = false;
                break;
            }
        }

        if (all_received) {
            NIXL_TRACE << "All XFER_IDs received for queued notification, processing now";

            // Move notification to main list (need to acquire notif_mutex_)
            {
                std::lock_guard<std::mutex> notif_lock(notif_mutex_);
                notifMainList_.push_back({it->remote_agent, it->message});
            }

            NIXL_TRACE << "Processed queued notification: " << it->message;

            // Remove from pending list
            it = pending_notifications_.erase(it);
        } else {
            ++it;
        }
    }
}

void
nixlLibfabricEngine::cleanup() {
    NIXL_DEBUG << "Cleaning up all resources";
#ifdef HAVE_CUDA
    // Cleanup CUDA context
    vramFiniCtx();
#endif

    NIXL_DEBUG << "Cleanup all resources complete";
}
