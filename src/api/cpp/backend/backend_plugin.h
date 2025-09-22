/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#ifndef __BACKEND_PLUGIN_H
#define __BACKEND_PLUGIN_H

#include "backend/backend_engine.h"
#include "common/nixl_log.h"

// Forward declarations for special engine types
class nixlUcxEngine;

// Define the plugin API version
#define NIXL_PLUGIN_API_VERSION 1

// Define the plugin interface class
class nixlBackendPlugin {
public:
    int api_version;

    // Function pointer for creating a new backend engine instance
    nixlBackendEngine* (*create_engine)(const nixlBackendInitParams* init_params);

    // Function pointer for destroying a backend engine instance
    void (*destroy_engine)(nixlBackendEngine* engine);

    // Function to get the plugin name
    const char* (*get_plugin_name)();

    // Function to get the plugin version
    const char* (*get_plugin_version)();

    // Function to get backend options
    nixl_b_params_t (*get_backend_options)();

    // Function to get supported backend mem types
    nixl_mem_list_t (*get_backend_mems)();
};

// Macro to define exported C functions for the plugin
#define NIXL_PLUGIN_EXPORT __attribute__((visibility("default")))

// Template for creating backend plugins with minimal boilerplate
template<typename EngineType> class nixlBackendPluginCreator {
public:
    static nixlBackendPlugin *
    create(int api_version,
           const char *name,
           const char *version,
           const nixl_b_params_t &params,
           const nixl_mem_list_t &mem_list) {

        static const char *plugin_name = name;
        static const char *plugin_version = version;
        static const nixl_b_params_t plugin_params = params;
        static const nixl_mem_list_t plugin_mems = mem_list;

        static nixlBackendPlugin plugin_instance = {api_version,
                                                    createEngine,
                                                    destroyEngine,
                                                    []() { return plugin_name; },
                                                    []() { return plugin_version; },
                                                    []() { return plugin_params; },
                                                    []() { return plugin_mems; }};

        return &plugin_instance;
    }

private:
    [[nodiscard]] static nixlBackendEngine *
    createEngine(const nixlBackendInitParams *init_params) {
        try {
            if constexpr (std::is_same_v<EngineType, nixlUcxEngine>) {
                // UCX engine uses a factory pattern
                auto engine = EngineType::create(*init_params);
                return engine.release();
            } else {
                // Other engines use direct constructor
                return new EngineType(init_params);
            }
        }
        catch (const std::exception &e) {
            NIXL_ERROR << "Failed to create engine: " << e.what();
            return nullptr;
        }
    }

    static void
    destroyEngine(nixlBackendEngine *engine) {
        delete engine;
    }
};


// Creator Function type for static plugins
typedef nixlBackendPlugin* (*nixlStaticPluginCreatorFunc)();

// Plugin must implement these functions for dynamic loading
// Note: extern "C" is required for dynamic loading to avoid C++ name mangling
extern "C" {
// Initialize the plugin
NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init();

// Cleanup the plugin
NIXL_PLUGIN_EXPORT void
nixl_plugin_fini();
}

#endif // __BACKEND_PLUGIN_H
