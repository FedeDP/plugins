// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2024 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#pragma once

#include <falcosecurity/sdk.h>
#include "num/cms.h"
#include "plugin_consts.h"
#include "plugin_utils.h"
#include "plugin_sinsp_filterchecks.h"
#include <driver/ppm_events_public.h> // Temporary workaround to avoid redefining syscalls PPME events and risking being out of sync

#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

# define UINT32_MAX		(4294967295U)

class anomalydetection
{
    public:
    // Keep this aligned with `get_fields`
    enum anomalydetection_fields
    {
        ANOMALYDETECTION_COUNT_MIN_SKETCH_COUNT = 0,
        ANOMALYDETECTION_COUNT_MIN_SKETCH_BEHAVIOR_PROFILE_CONCAT_STR,
        ANOMALYDETECTION_FIELD_MAX
    };

    //////////////////////////
    // General plugin API
    //////////////////////////

    virtual ~anomalydetection() = default;

    std::string get_name() { return PLUGIN_NAME; }

    std::string get_version() { return PLUGIN_VERSION; }

    std::string get_description() { return PLUGIN_DESCRIPTION; }

    std::string get_contact() { return PLUGIN_CONTACT; }

    std::string get_required_api_version()
    {
        return PLUGIN_REQUIRED_API_VERSION;
    }

    falcosecurity::init_schema get_init_schema();

    void parse_init_config(nlohmann::json& config_json);

    bool init(falcosecurity::init_input& in);

    // todo
    // void destroy();

    std::string get_last_error() { return m_lasterr; }

    void log_error(std::string err_mess);

    //////////////////////////
    // Extract capability
    //////////////////////////

    // required; standard plugin API
    std::vector<std::string> get_extract_event_sources()
    {
        return {"syscall"};
    }

    // required; standard plugin API
    std::vector<falcosecurity::field_info> get_fields();

    // required; standard plugin API
    bool extract(const falcosecurity::extract_fields_input& in);

    //////////////////////////
    // Parse capability
    //////////////////////////

    // required; standard plugin API
    std::vector<std::string> get_parse_event_sources()
    {
        return {"syscall"};
    }

    // required; standard plugin API
    std::vector<falcosecurity::event_type> get_parse_event_types()
    {
        std::vector<falcosecurity::event_type> event_types;
        // Temporary workaround
        for (int i = PPME_GENERIC_E; i <= PPM_EVENT_MAX; ++i) 
        {
            event_types.push_back(static_cast<falcosecurity::event_type>(i));
        }
        return event_types;
    }

    // required; standard plugin API
    bool parse_event(const falcosecurity::parse_event_input& in);

    // Custom helper function within event parsing
    bool extract_filterchecks_concat_profile(int64_t thread_id, const falcosecurity::table_reader &tr, const std::vector<plugin_sinsp_filterchecks_field>& fields, std::string& behavior_profile_concat_str);

    private:

    bool m_count_min_sketch_enabled = false;
    uint32_t m_n_sketches = 0;
    std::vector<std::vector<double>> m_gamma_eps;
    std::vector<std::vector<uint64_t>> m_rows_cols; // If set supersedes m_gamma_eps
    std::vector<std::vector<plugin_sinsp_filterchecks_field>> m_behavior_profiles_fields;
    std::vector<std::unordered_set<ppm_event_code>> m_behavior_profiles_event_codes;

    // Plugin managed state table
    std::vector<std::unique_ptr<plugin::anomalydetection::num::cms<uint64_t>>> m_count_min_sketches;

    // required; standard plugin API
    std::string m_lasterr;
    // required; standard plugin API; accessor to falcosecurity/libs' thread table
    falcosecurity::table m_thread_table;
    // Accessors to the fixed fields of falcosecurity/libs' thread table -> non comprehensive re-definition of sinsp_threadinfo
    // Reference in falcosecurity/libs: userspace/libsinsp/threadinfo.h
    falcosecurity::table_field m_tid; ///< The id of this thread
    falcosecurity::table_field m_pid; ///< The id of the process containing this thread. In single thread threads, this is equal to tid.
    falcosecurity::table_field m_ptid; ///< The id of the process that started this thread.
    falcosecurity::table_field m_sid; ///< The session id of the process containing this thread.
    falcosecurity::table_field m_comm; ///< Command name (e.g. "top")
    falcosecurity::table_field m_exe; ///< argv[0] (e.g. "sshd: user@pts/4")
    falcosecurity::table_field m_exepath; ///< full executable path
    falcosecurity::table_field m_exe_writable;
    falcosecurity::table_field m_exe_upper_layer; ///< True if the executable file belongs to upper layer in overlayfs
    falcosecurity::table_field m_exe_from_memfd; ///< True if the executable is stored in fileless memory referenced by memfd
    falcosecurity::table_field m_args; ///< Command line arguments (e.g. "-d1")
    falcosecurity::table_field m_args_value; ///< String value entry from the args array
    falcosecurity::table_field m_env; ///< Environment variables
    falcosecurity::table_field m_container_id; ///< heuristic-based container id
    falcosecurity::table_field m_uid; ///< user uid
    falcosecurity::table_field m_user; ///< user infos
    falcosecurity::table_field m_loginuid; ///< auid
    falcosecurity::table_field m_loginuser; ///< loginuser infos (auid)
    falcosecurity::table_field m_group; ///< group infos
    falcosecurity::table_field m_vtid; ///< The virtual id of this thread.
    falcosecurity::table_field m_vpid; ///< The virtual id of the process containing this thread. In single thread threads, this is equal to vtid.
    falcosecurity::table_field m_vpgid; // The virtual process group id, as seen from its pid namespace
    falcosecurity::table_field m_tty; ///< Number of controlling terminal
    falcosecurity::table_field m_cwd; ///< current working directory
};

// required; standard plugin API
FALCOSECURITY_PLUGIN(anomalydetection);
FALCOSECURITY_PLUGIN_FIELD_EXTRACTION(anomalydetection);
FALCOSECURITY_PLUGIN_EVENT_PARSING(anomalydetection);
