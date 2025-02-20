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

#include "plugin.h"

#include <optional>
#include <filesystem>
#include <sys/stat.h>

void anomalydetection::log_error(std::string err_mess)
{
    printf("%s %s\n", PLUGIN_LOG_PREFIX, err_mess.c_str());
}

//////////////////////////
// Initializations
//////////////////////////

falcosecurity::init_schema anomalydetection::get_init_schema()
{
    falcosecurity::init_schema init_schema;
    init_schema.schema_type =
            falcosecurity::init_schema_type::SS_PLUGIN_SCHEMA_JSON;
    init_schema.schema = R"(
{
  "$schema": "http://json-schema.org/draft-04/schema#",
  "type": "object",
  "properties": {
    "count_min_sketch": {
      "type": "object",
      "properties": {
        "enabled": {
          "type": "boolean"
        },
        "n_sketches": {
          "type": "integer",
          "minimum": 1
        },
        "gamma_eps": {
          "type": "array",
          "items": {
            "type": "array",
            "items": [
              {
                "type": "number"
              },
              {
                "type": "number"
              }
            ],
            "minItems": 0,
            "maxItems": 2
          },
          "minItems": 1
        },
        "rows_cols": {
          "type": "array",
          "items": {
            "type": "array",
            "items": [
              {
                "type": "number"
              },
              {
                "type": "number"
              }
            ],
            "minItems": 2,
            "maxItems": 2
          },
          "minItems": 1
        },
        "behavior_profiles": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "fields": {
                "type": "string",
                "description": "The anomalydetection behavior profile string including the fields."
              },
              "event_codes": {
                "type": "array",
                "description": "The list of PPME event codes to which the behavior profile updates should be applied.",
                "items": {
                  "type": "number",
                  "description": "PPME event codes supported by Falco."
                }
              },
              "reset_timer_ms": {
                "type": "number",
                "description": "The anomaly detection behavior profile timer, in milliseconds (ms), is used to reset the sketch counts."
              }
            },
            "required": [
              "fields",
              "event_codes"
            ]
          },
          "minItems": 1
        }
      }
    }
  }
})";
    return init_schema;
}

void anomalydetection::parse_init_config(nlohmann::json& config_json)
{
    // Clear in case of config hot reloads
    m_gamma_eps.clear();
    m_rows_cols.clear();
    m_reset_timers.clear();
    m_behavior_profiles_fields.clear();
    m_behavior_profiles_event_codes.clear();
    if(config_json.contains(nlohmann::json::json_pointer("/count_min_sketch")))
    {
        if(config_json.contains(nlohmann::json::json_pointer("/count_min_sketch/enabled")))
        {
            config_json.at(nlohmann::json::json_pointer("/count_min_sketch/enabled"))
                    .get_to(m_count_min_sketch_enabled);
        }

        // don't use an early return approach as configs may span beyond count_min_sketch in the future
        if (m_count_min_sketch_enabled)
        {
            // Config JSON schema enforces a minimum of 1 sketches
            if(config_json.contains(nlohmann::json::json_pointer("/count_min_sketch/n_sketches")))
            {
                config_json.at(nlohmann::json::json_pointer("/count_min_sketch/n_sketches"))
                        .get_to(m_n_sketches);
            }

            // If used, config JSON schema enforces a minimum of 1 items and 2-d sub-arrays
            auto gamma_eps_pointer = nlohmann::json::json_pointer("/count_min_sketch/gamma_eps");
            if (config_json.contains(gamma_eps_pointer) && config_json[gamma_eps_pointer].is_array())
            {
                int n = 1;
                for (const auto& array : config_json[gamma_eps_pointer])
                {
                    if (array.is_array() && array.size() == 2)
                    {
                        std::vector<double> sub_array = {array[0].get<double>(), array[1].get<double>()};
                        log_error("Count min sketch data structure number (" 
                        + std::to_string(n) + ") loaded with gamma and eps values (" 
                        + std::to_string(sub_array[0]) + ","
                        + std::to_string(sub_array[1])
                        + ") equivalent to sketch dimensions ("
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::calculate_d_rows_from_gamma(sub_array[0])) + ","
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::calculate_w_cols_buckets_from_eps(sub_array[1]))
                        + ") -> adding ("
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::get_size_bytes(plugin::anomalydetection::num::cms<uint64_t>::calculate_d_rows_from_gamma(sub_array[0]),plugin::anomalydetection::num::cms<uint64_t>::calculate_w_cols_buckets_from_eps(sub_array[1])))
                        + ") bytes of constant memory allocation on the heap");
                        m_gamma_eps.emplace_back(sub_array);     
                    }
                    n++;
                }
            }

            // If used, config JSON schema enforces a minimum of 1 items and 2-d sub-arrays
            auto rows_cols_pointer = nlohmann::json::json_pointer("/count_min_sketch/rows_cols");
            if (config_json.contains(rows_cols_pointer) && config_json[rows_cols_pointer].is_array())
            {
                int n = 1;
                if (config_json.contains(gamma_eps_pointer) && config_json[gamma_eps_pointer].is_array())
                {
                    log_error("[Override Notice] Count min sketch data structures will be overriden with below settings as 'rows_cols' config overrides any previous setting");
                }
                for (const auto& array : config_json[rows_cols_pointer])
                {
                    if (array.is_array() && array.size() == 2)
                    {
                        std::vector<uint64_t> sub_array = {array[0].get<uint64_t>(), array[1].get<uint64_t>()};
                        log_error("Count min sketch data structure number (" 
                        + std::to_string(n) + ") loaded with d and w/buckets values (" 
                        + std::to_string(sub_array[0]) + ","
                        + std::to_string(sub_array[1])
                        + ") equivalent to sketch error probability and relative error tolerances ("
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::calculate_gamma_rows_from_d(sub_array[0])) + ","
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::calculate_eps_cols_buckets_from_w(sub_array[1]))
                        + ") -> adding ("
                        + std::to_string(plugin::anomalydetection::num::cms<uint64_t>::get_size_bytes(sub_array[0],sub_array[1]))
                        + ") bytes of constant memory allocation on the heap");
                        m_rows_cols.emplace_back(sub_array);
                    }
                    n++;
                }
            }

            // Config JSON schema enforces a minimum of 1 item
            auto behavior_profiles_pointer = nlohmann::json::json_pointer("/count_min_sketch/behavior_profiles");
            if (config_json.contains(behavior_profiles_pointer) && config_json[behavior_profiles_pointer].is_array())
            {
                const auto& behavior_profiles = config_json[behavior_profiles_pointer];
                const std::vector<ppm_event_code> supported_codes_fd_profile = {
                    PPME_SYSCALL_OPEN_X,
                    PPME_SOCKET_ACCEPT_5_X,
                    PPME_SOCKET_ACCEPT4_6_X,
                    PPME_SYSCALL_CREAT_X,
                    PPME_SOCKET_CONNECT_X,
                    PPME_SYSCALL_OPENAT_2_X,
                    PPME_SYSCALL_OPENAT2_X,
                    PPME_SYSCALL_OPEN_BY_HANDLE_AT_X
                };
                std::vector<ppm_event_code> supported_codes_any_profile = {
                    PPME_SYSCALL_EXECVEAT_X,
                    PPME_SYSCALL_EXECVE_19_X,
                    PPME_SYSCALL_CLONE_20_X,
                    PPME_SYSCALL_CLONE3_X
                };
                supported_codes_any_profile.insert(
                    supported_codes_any_profile.end(),
                    supported_codes_fd_profile.begin(),
                    supported_codes_fd_profile.end()
                );

                int n = 1;
                for (const auto& profile : behavior_profiles)
                {
                    std::vector<plugin_sinsp_filterchecks_field> filter_check_fields;
                    std::unordered_set<ppm_event_code> codes;
                    if (profile.contains("fields") && profile.contains("event_codes"))
                    {
                        filter_check_fields = plugin_anomalydetection::utils::get_profile_fields(profile["fields"].get<std::string>());
                        std::ostringstream oss;
                        bool first_event_code = true;
                        for (const auto& code : profile["event_codes"])
                        {
                            codes.insert((ppm_event_code)code.get<uint64_t>());
                            if (!first_event_code)
                            {
                                oss << ",";
                            }
                            oss << code;
                            first_event_code = false;
                        }
                        std::string event_codes_string = oss.str();
                        log_error("Behavior profile number (" + std::to_string(n) + ") loaded and applied to event_codes (" + event_codes_string + ") with behavior profile fields (" + profile["fields"].get<std::string>() + ")");

                        /* Some rudimentary initial checks to ensure profiles with %fd fields are applied on fd related events only */
                        if (profile["fields"].get<std::string>().find("%fd") != std::string::npos)
                        {
                            for (const auto& code : codes)
                            {
                                if (std::find(supported_codes_fd_profile.begin(), supported_codes_fd_profile.end(), code) == supported_codes_fd_profile.end())
                                {
                                    log_error("The above behavior profile contains '%fd' related fields but includes non fd related event codes such as code (" + std::to_string(code) + "), which is not allowed. Please refer to the docs for assistance, exiting...");
                                    exit(1);
                                }
                            }
                        }
                        /* Some rudimentary checks to generally limit the event codes to a subset of supported event codes. */
                        for (const auto& code : codes)
                        {
                            if (std::find(supported_codes_any_profile.begin(), supported_codes_any_profile.end(), code) == supported_codes_any_profile.end())
                            {
                                log_error("The above behavior profile contains event codes such as code (" + std::to_string(code) + ") that are currently not at all allowed for behavior profiles. Please refer to the docs for assistance, exiting...");
                                exit(1);
                            }
                        }
                    }
                    if (profile.contains("reset_timer_ms"))
                    {
                        uint64_t interval = profile["reset_timer_ms"].get<uint64_t>();
                        if (interval > 100)
                        {
                            m_reset_timers.emplace_back(interval);
                        } else
                        {
                            m_reset_timers.emplace_back(uint64_t(0));
                        }
                        log_error("Behavior profile number (" + std::to_string(n) + ") resets the counts to zero every (" + std::to_string(interval) + ") ms");
                    } else
                    {
                        m_reset_timers.emplace_back(uint64_t(0));
                    }
                    m_behavior_profiles_fields.emplace_back(filter_check_fields);
                    m_behavior_profiles_event_codes.emplace_back(std::move(codes));
                    n++;
                }
            }

            // Check correlated conditions that can't be directly enforced by the config JSON schema
            if (!m_gamma_eps.empty() && m_n_sketches != m_gamma_eps.size())
            {
                log_error("Config gamma_eps needs to match the specified number of sketches");
                assert(false);
            }
            if (!m_rows_cols.empty() && m_n_sketches != m_rows_cols.size())
            {
                log_error("Config rows_cols needs to match the specified number of sketches");
                assert(false);
            }
            if (m_n_sketches != m_behavior_profiles_fields.size())
            {
                log_error("Config behavior_profiles needs to match the specified number of sketches");
                assert(false);
            }
            if (m_n_sketches != m_behavior_profiles_event_codes.size())
            {
                log_error("Config behavior_profiles needs to match the specified number of sketches");
                assert(false);
            }
        }
    }
}

bool anomalydetection::init(falcosecurity::init_input& in)
{
    using st = falcosecurity::state_value_type;
    auto& t = in.tables();

    // This should never happen, the config is validated by the framework
    if(in.get_config().empty())
    {
        return false;
    }

    auto cfg = nlohmann::json::parse(in.get_config());
    parse_init_config(cfg);

    //////////////////////////
    // Init fields
    //////////////////////////

    try
    {
        // Accessor to falcosecurity/libs' thread table (process cache / core state engine)
        m_thread_table = t.get_table(THREAD_TABLE_NAME, st::SS_PLUGIN_ST_INT64);
        // Define accessors to falcosecurity/libs' thread table fields

        /* Subtables */
        m_args = m_thread_table.get_field(t.fields(), "args", st::SS_PLUGIN_ST_TABLE);
        m_env = m_thread_table.get_field(t.fields(), "env", st::SS_PLUGIN_ST_TABLE);
        m_fds = m_thread_table.get_field(t.fields(), "file_descriptors", st::SS_PLUGIN_ST_TABLE);

        /* proc related */
        m_tid = m_thread_table.get_field(t.fields(), "tid", st::SS_PLUGIN_ST_INT64);
        m_pid = m_thread_table.get_field(t.fields(), "pid", st::SS_PLUGIN_ST_INT64);
        m_ptid = m_thread_table.get_field(t.fields(), "ptid", st::SS_PLUGIN_ST_INT64);
        m_sid = m_thread_table.get_field(t.fields(), "sid", st::SS_PLUGIN_ST_INT64);
        m_comm = m_thread_table.get_field(t.fields(), "comm", st::SS_PLUGIN_ST_STRING);
        m_exe = m_thread_table.get_field(t.fields(), "exe", st::SS_PLUGIN_ST_STRING);
        m_exepath = m_thread_table.get_field(t.fields(), "exe_path", st::SS_PLUGIN_ST_STRING);
        m_exe_writable = m_thread_table.get_field(t.fields(), "exe_writable", st::SS_PLUGIN_ST_BOOL);
        m_exe_upper_layer = m_thread_table.get_field(t.fields(), "exe_upper_layer", st::SS_PLUGIN_ST_BOOL);
        m_exe_from_memfd = m_thread_table.get_field(t.fields(), "exe_from_memfd", st::SS_PLUGIN_ST_BOOL);
        m_exe_ino = m_thread_table.get_field(t.fields(), "exe_ino", st::SS_PLUGIN_ST_UINT64);
        m_exe_ino_ctime = m_thread_table.get_field(t.fields(), "exe_ino_ctime", st::SS_PLUGIN_ST_UINT64);
        m_exe_ino_mtime = m_thread_table.get_field(t.fields(), "exe_ino_mtime", st::SS_PLUGIN_ST_UINT64);
        m_args_value = t.get_subtable_field(m_thread_table, m_args, "value", st::SS_PLUGIN_ST_STRING);
        m_env_value = t.get_subtable_field(m_thread_table, m_env, "value", st::SS_PLUGIN_ST_STRING);
        m_vtid = m_thread_table.get_field(t.fields(), "vtid", st::SS_PLUGIN_ST_INT64);
        m_vpid = m_thread_table.get_field(t.fields(), "vpid", st::SS_PLUGIN_ST_INT64);
        m_vpgid = m_thread_table.get_field(t.fields(), "vpgid", st::SS_PLUGIN_ST_INT64);
        m_tty = m_thread_table.get_field(t.fields(), "tty", st::SS_PLUGIN_ST_UINT32);
        m_cwd = m_thread_table.get_field(t.fields(), "cwd", st::SS_PLUGIN_ST_STRING);

        /* user related */
        // Not available until the next libs plugins API expansion

        // m_user = m_thread_table.get_field(t.fields(), "user", TBD);
        // m_loginuser = m_thread_table.get_field(t.fields(), "loginuser", TBD);
        // m_group = m_thread_table.get_field(t.fields(), "group", TBD);

        /* fd related */
        // m_fd_type_value = t.get_subtable_field(m_thread_table, m_fds, "type", st::SS_PLUGIN_ST_UINT32); // todo fix/expose via plugin API, likely type issue given its of type scap_fd_type
        m_fd_openflags_value = t.get_subtable_field(m_thread_table, m_fds, "open_flags", st::SS_PLUGIN_ST_UINT32);
        // m_fd_sockinfo_value = t.get_subtable_field(m_thread_table, m_fds, "sock_info", st::SS_PLUGIN_ST_UINT32); // todo fix/expose via plugin API, likely type issue given its of type sinsp_sockinfo
        m_fd_name_value = t.get_subtable_field(m_thread_table, m_fds, "name", st::SS_PLUGIN_ST_STRING);
        m_fd_nameraw_value = t.get_subtable_field(m_thread_table, m_fds, "name_raw", st::SS_PLUGIN_ST_STRING);
        m_fd_oldname_value = t.get_subtable_field(m_thread_table, m_fds, "old_name", st::SS_PLUGIN_ST_STRING);
        m_fd_flags_value = t.get_subtable_field(m_thread_table, m_fds, "flags", st::SS_PLUGIN_ST_UINT32);
        m_fd_dev_value = t.get_subtable_field(m_thread_table, m_fds, "dev", st::SS_PLUGIN_ST_UINT32);
        m_fd_mount_id_value = t.get_subtable_field(m_thread_table, m_fds, "mount_id", st::SS_PLUGIN_ST_UINT32);
        m_fd_ino_value = t.get_subtable_field(m_thread_table, m_fds, "ino", st::SS_PLUGIN_ST_UINT64);
        m_fd_pid_value = t.get_subtable_field(m_thread_table, m_fds, "pid", st::SS_PLUGIN_ST_INT64);
        // m_fd_fd_value = t.get_subtable_field(m_thread_table, m_fds, "fd", st::SS_PLUGIN_ST_INT64);

        /* container related */
        m_container_id = m_thread_table.get_field(t.fields(), "container_id", st::SS_PLUGIN_ST_STRING);

        /* Custom fields */
        m_lastevent_fd_field = m_thread_table.add_field(
                t.fields(), "lastevent_fd", st::SS_PLUGIN_ST_INT64);
    }
    catch(falcosecurity::plugin_exception e)
    {
        m_lasterr = "cannot init libs' thread table info fields: '" + std::string(e.what());
        return false;
    }

    ////////////////////
    // Init sketches
    ////////////////////

    // More custom inits
    struct stat st_ = {0};
    if(stat("/proc/self/cmdline", &st_) == 0)
    {
        m_falco_start_ts_epoch_ns = st_.st_ctim.tv_sec * SECOND_TO_NS + st_.st_ctim.tv_nsec;
    }

    // Init the plugin managed state table holding the count min sketch estimates for each behavior profile
    m_thread_manager.stop_threads(); // Important for reloading configs conditions
    m_count_min_sketches.lock()->clear();

    if (m_count_min_sketch_enabled)
    {
        if (m_rows_cols.size() == m_n_sketches)
        {
            for (uint32_t i = 0; i < m_n_sketches; ++i)
            {
                uint64_t rows = m_rows_cols[i][0];
                uint64_t cols = m_rows_cols[i][1];
                m_count_min_sketches.lock()->push_back(std::make_shared<plugin::anomalydetection::num::cms<uint64_t>>(rows, cols));
            }
        } else if (m_gamma_eps.size() == m_n_sketches && m_rows_cols.empty())
        {
            for (uint32_t i = 0; i < m_n_sketches; ++i)
            {
                double gamma = m_gamma_eps[i][0];
                double eps = m_gamma_eps[i][1];
                m_count_min_sketches.lock()->push_back(std::make_shared<plugin::anomalydetection::num::cms<uint64_t>>(gamma, eps));
            }
        } else
        {
            return false;
        }

        // Launch threads to periodically reset the data structures (if applicable)
        m_thread_manager.m_stop_requested = false;
        for (uint32_t i = 0; i < m_n_sketches; ++i)
        {
            m_thread_manager.start_periodic_count_min_sketch_reset_worker<uint64_t>(i, (uint64_t)m_reset_timers[i], m_count_min_sketches);
        }
    }

    return true;
}

//////////////////////////
// Extract capability
//////////////////////////

std::vector<falcosecurity::field_info> anomalydetection::get_fields()
{
    using ft = falcosecurity::field_value_type;
    const falcosecurity::field_info fields[] = {
            {ft::FTYPE_UINT64, "anomaly.count_min_sketch", 
             "Count Min Sketch Estimate",
             "Count Min Sketch Estimate according to the specified behavior profile for a predefined set of {syscalls} events. Access different behavior profiles/sketches using indices. For instance, anomaly.count_min_sketch[0] retrieves the first behavior profile defined in the plugins' `init_config`.",
             { // field arg
                false, // key
                true,  // index
                false,
             }},
            {ft::FTYPE_STRING, "anomaly.count_min_sketch.profile", 
             "Behavior Profile Concatenated String",
             "Concatenated string according to the specified behavior profile (not preserving original order). Access different behavior profiles using indices. For instance, anomaly.count_min_sketch.profile[0] retrieves the first behavior profile defined in the plugins' `init_config`.",
             { // field arg
                false, // key
                true,  // index
                false,
             }},
             {ft::FTYPE_UINT64, "anomaly.falco.duration_ns", 
             "Falco agent run duration in nanoseconds",
             "Falco agent run duration in nanoseconds, which could be useful for ignoring some rare events at launch time while Falco is just starting to build up the counts in the sketch data structures (if applicable).",
             { // field arg
                false, // key
                false,  // index
                false,
             }},
    };
    const int fields_size = sizeof(fields) / sizeof(fields[0]);
    static_assert(fields_size == ANOMALYDETECTION_FIELD_MAX, "Wrong number of anomaly fields.");
    return std::vector<falcosecurity::field_info>(fields, fields + fields_size);
}

bool anomalydetection::extract(const falcosecurity::extract_fields_input& in)
{
    auto& req = in.get_extract_request();
    auto& tr = in.get_table_reader();
    auto& evt = in.get_event_reader();
    switch(req.get_field_id())
    {
    case ANOMALYDETECTION_COUNT_MIN_SKETCH_COUNT:
        {
            int64_t thread_id = evt.get_tid();
            uint64_t count_min_sketch_estimate = 0;
            std::string behavior_profile_concat_str;
            auto index = req.get_arg_index();
            if(!m_count_min_sketch_enabled)
            {
                m_lasterr = "count_min_sketch disabled, but `anomaly.count_min_sketch` field referenced";
                return false;
            }
            if(index >= m_n_sketches)
            {
                m_lasterr = "sketch index out of bounds";
                return false;
            }
            if(extract_filterchecks_concat_profile(evt, tr, m_behavior_profiles_fields[index], behavior_profile_concat_str))
            {
                count_min_sketch_estimate = m_count_min_sketches.lock()->at(index).get()->estimate(behavior_profile_concat_str);
                req.set_value(count_min_sketch_estimate, true);
            }
            return true;
        }
    case ANOMALYDETECTION_COUNT_MIN_SKETCH_BEHAVIOR_PROFILE_CONCAT_STR:
        {
            int64_t thread_id = evt.get_tid();
            uint64_t count_min_sketch_estimate = 0;
            std::string behavior_profile_concat_str;
            auto index = req.get_arg_index();
            if(!m_count_min_sketch_enabled)
            {
                m_lasterr = "count_min_sketch disabled, but `anomaly.count_min_sketch` field referenced";
                return false;
            }
            if(index >= m_n_sketches)
            {
                m_lasterr = "sketch index out of bounds";
                return false;
            }
            if(extract_filterchecks_concat_profile(evt, tr, m_behavior_profiles_fields[index], behavior_profile_concat_str))
            {
                req.set_value(behavior_profile_concat_str, true);
            }
            return true;
        }  
    case ANOMALYDETECTION_FALCO_DURATION_NS:
        {
            auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            req.set_value((uint64_t)(now - m_falco_start_ts_epoch_ns), true);
        }
        return true;
    default:
        m_lasterr = "unknown extraction request";
        return false;
    }

    return true;
}

//////////////////////////
// Parse capability
//////////////////////////

// Adopted from the k8smeta plugin, obtain a param from a sinsp event
static inline sinsp_param get_syscall_evt_param(void* evt, uint32_t num_param)
{
    uint32_t dataoffset = 0;
    // pointer to the lengths array inside the event.
    auto len = (uint16_t*)((uint8_t*)evt +
                           sizeof(falcosecurity::_internal::ss_plugin_event));
    for(uint32_t j = 0; j < num_param; j++)
    {
        // sum lengths of the previous params.
        dataoffset += len[j];
    }
    return {.param_len = len[num_param],
            .param_pointer =
                    ((uint8_t*)&len
                             [((falcosecurity::_internal::ss_plugin_event*)evt)
                                      ->nparams]) +
                    dataoffset};
}

std::string anomalydetection::extract_filterchecks_evt_params_fallbacks(const falcosecurity::event_reader &evt, const plugin_sinsp_filterchecks_field& field, const std::string& cwd)
{
    using st = falcosecurity::state_value_type;

    std::string tstr;
    uint64_t tuint64 = UINT64_MAX;
    uint32_t tuint32 = UINT32_MAX;
    int64_t tint64 = -1;
    bool tbool;
    int64_t ptid = -1;
    switch(field.id)
    {

    //
    // fd related
    //

    case plugin_sinsp_filterchecks::TYPE_FDNUM:
    {
        switch(evt.get_type())
        {
        case PPME_SYSCALL_OPEN_X:
        case PPME_SYSCALL_CREAT_X:
        case PPME_SYSCALL_OPENAT_2_X:
        case PPME_SYSCALL_OPENAT2_X:
        case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
        case PPME_SOCKET_ACCEPT_5_X:
        case PPME_SOCKET_ACCEPT4_6_X:
        {
            auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                0);
            if (res_param.param_pointer == nullptr)
            {
                return tstr;
            }
            int64_t fd = *(int64_t*)(res_param.param_pointer);
            tstr = std::to_string(fd);
            break;
        }
        case PPME_SOCKET_CONNECT_X:
        {
            auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                2);
            if (res_param.param_pointer == nullptr)
            {
                return tstr;
            }
            int64_t fd = *(int64_t*)(res_param.param_pointer);
            tstr = std::to_string(fd);
            break;
        }
        default:
            break;
        }
        break;
    }
    case plugin_sinsp_filterchecks::TYPE_FDNAME:
    case plugin_sinsp_filterchecks::TYPE_DIRECTORY:
    case plugin_sinsp_filterchecks::TYPE_FILENAME:
    {
        switch(evt.get_type())
        {
        case PPME_SYSCALL_OPEN_X:
        case PPME_SYSCALL_CREAT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                1);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
                if(!std::filesystem::path(tstr).is_absolute())
                {
                    tstr = plugin_anomalydetection::utils::concatenate_paths(cwd, tstr);
                } else
                {
                    // concatenate_paths takes care of resolving the path
                    tstr = plugin_anomalydetection::utils::concatenate_paths("", tstr);
                }
            }
            break;
        case PPME_SYSCALL_OPENAT_2_X:
        case PPME_SYSCALL_OPENAT2_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                2);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
                // cwd passed to the function here is the name extracted from the dirfd
                tstr = plugin_anomalydetection::utils::concatenate_paths(cwd, tstr);
            }
            break;
        case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                3);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
                tstr = plugin_anomalydetection::utils::concatenate_paths("", tstr);
            }
            break;
        case PPME_SOCKET_ACCEPT_5_X:
        case PPME_SOCKET_ACCEPT4_6_X:
        case PPME_SOCKET_CONNECT_X:
        {
            // todo fix/expose via plugin API fallbacks as we lack access to sockinfo and it's highly more sophisticated / complex
            // auto res_param = get_syscall_evt_param(evt.get_buf(),
            //                                     1);
            // if (res_param.param_pointer == nullptr)
            // {
            //     return tstr;
            // }
            break;
        }
        default:
            break;
        }
        break;
    }
    case plugin_sinsp_filterchecks::TYPE_INO:
    {
        switch(evt.get_type())
        {
        case PPME_SYSCALL_OPEN_X:
        case PPME_SYSCALL_CREAT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                5);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint64_t ino = *(uint64_t*)(res_param.param_pointer);
                tstr = std::to_string(ino);
            }
            break;
        case PPME_SYSCALL_OPENAT_2_X:
        case PPME_SYSCALL_OPENAT2_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                7);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint64_t ino = *(uint64_t*)(res_param.param_pointer);
                tstr = std::to_string(ino);
            }
            break;
        case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                5);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint64_t ino = *(uint64_t*)(res_param.param_pointer);
                tstr = std::to_string(ino);
            }
            break;
        default:
            break;
        }
        break;
    }
    case plugin_sinsp_filterchecks::TYPE_DEV:
    {
        switch(evt.get_type())
        {
        case PPME_SYSCALL_OPEN_X:
        case PPME_SYSCALL_CREAT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                4);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint32_t dev = *(uint32_t*)(res_param.param_pointer);
                tstr = std::to_string(dev);
            }
            break;
        case PPME_SYSCALL_OPENAT_2_X:
        case PPME_SYSCALL_OPENAT2_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                6);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint32_t dev = *(uint32_t*)(res_param.param_pointer);
                tstr = std::to_string(dev);
            }
            break;
        case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                4);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                uint32_t dev = *(uint32_t*)(res_param.param_pointer);
                tstr = std::to_string(dev);
            }
            break;
        default:
            break;
        }
        break;
    }
    case plugin_sinsp_filterchecks::TYPE_FDNAMERAW:
    {
        switch(evt.get_type())
        {
        case PPME_SYSCALL_OPEN_X:
        case PPME_SYSCALL_CREAT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                1);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
            }
            break;
        case PPME_SYSCALL_OPENAT_2_X:
        case PPME_SYSCALL_OPENAT2_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                2);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
            }
            break;
        case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                3);
                if (res_param.param_pointer == nullptr)
                {
                    return tstr;
                }
                tstr = (char*)(res_param.param_pointer);
            }
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return tstr;
}

bool anomalydetection::extract_filterchecks_concat_profile(const falcosecurity::event_reader &evt, const falcosecurity::table_reader &tr, const std::vector<plugin_sinsp_filterchecks_field>& fields, std::string& behavior_profile_concat_str)
{
    using st = falcosecurity::state_value_type;

    int64_t thread_id = evt.get_tid();
    std::string tstr;
    std::optional<falcosecurity::table_entry> thread_entry_opt;
    try
    {
        thread_entry_opt = m_thread_table.get_entry(tr, thread_id);
    } catch (const std::exception& e)
    {
        for (const auto& field : fields)
        {
            behavior_profile_concat_str += extract_filterchecks_evt_params_fallbacks(evt, field);
        }
        return true;
    }
    auto& thread_entry = thread_entry_opt.value();

    // Create a concatenated string formed out of each field per behavior profile
    // No concept of null fields (instead its always an empty string) compared to libsinsp
    for (const auto& field : fields)
    {
        tstr.clear();
        uint64_t tuint64 = UINT64_MAX;
        uint32_t tuint32 = UINT32_MAX;
        int64_t tint64 = -1;
        bool tbool;
        int64_t ptid = -1;
        switch(field.id)
        {
        case plugin_sinsp_filterchecks::TYPE_CONTAINER_ID:
            m_container_id.read_value(tr, thread_entry, tstr);
            break;
        case plugin_sinsp_filterchecks::TYPE_NAME:
            m_comm.read_value(tr, thread_entry, tstr);
            break;
        case plugin_sinsp_filterchecks::TYPE_PNAME:
        {
            m_ptid.read_value(tr, thread_entry, ptid);
            auto lineage = m_thread_table.get_entry(tr, ptid);
            m_comm.read_value(tr, lineage, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_ANAME:
        {
            // todo: check implications of main thread as it's part of the libs implementation
            if(field.argid < 1)
            {
                m_comm.read_value(tr, thread_entry, tstr);
                break;
            }
            m_ptid.read_value(tr, thread_entry, ptid);
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    if(j == (field.argid - 1))
                    {
                        m_comm.read_value(tr, lineage, tstr);
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_ARGS:
        {
            const char* arg = nullptr;
            auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                {
                    arg = nullptr;
                    m_args_value.read_value(tr, e, arg);
                    if (!tstr.empty())
                    {
                        tstr += " ";
                    }
                    if (arg)
                    {
                        tstr += arg;
                    }
                    return true;
                });
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CMDNARGS:
        {
            const char* arg = nullptr;
            size_t c = 0;
            auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &c](const falcosecurity::table_entry& e)
                {
                    c++;
                    return true;
                });
            tstr = std::to_string(c);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CMDLENARGS:
        {
            const char* arg = nullptr;
            size_t c = 0;
            auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &tr, &arg, &c](const falcosecurity::table_entry& e)
                {
                    arg = nullptr;
                    m_args_value.read_value(tr, e, arg);
                    c+=std::strlen(arg);
                    return true;
                });
            tstr = std::to_string(c);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CMDLINE:
        {
            m_comm.read_value(tr, thread_entry, tstr);
            const char* arg = nullptr;
            auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                {
                    arg = nullptr;
                    m_args_value.read_value(tr, e, arg);
                    if (!tstr.empty())
                    {
                        tstr += " ";
                    }
                    if (arg)
                    {
                        tstr += arg;
                    }
                    return true;
                });
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_PCMDLINE:
        {
            m_ptid.read_value(tr, thread_entry, ptid);
            auto lineage = m_thread_table.get_entry(tr, ptid);
            m_comm.read_value(tr, lineage, tstr);
            const char* arg = nullptr;
            auto args_table = m_thread_table.get_subtable(tr, m_args, lineage, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                {
                    arg = nullptr;
                    m_args_value.read_value(tr, e, arg);
                    if (!tstr.empty())
                    {
                        tstr += " ";
                    }
                    if (arg)
                    {
                        tstr += arg;
                    }
                    return true;
                });
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_ACMDLINE:
        {
            if(field.argid < 1)
            {
                m_comm.read_value(tr, thread_entry, tstr);
                const char* arg = nullptr;
                auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
                args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                    {
                        arg = nullptr;
                        m_args_value.read_value(tr, e, arg);
                        if (!tstr.empty())
                        {
                            tstr += " ";
                        }
                        if (arg)
                        {
                            tstr += arg;
                        }
                        return true;
                    });
                break;
            }
            m_ptid.read_value(tr, thread_entry, ptid);
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    if(j == (field.argid - 1))
                    {
                        m_comm.read_value(tr, lineage, tstr);
                        const char* arg = nullptr;
                        auto args_table = m_thread_table.get_subtable(tr, m_args, lineage, st::SS_PLUGIN_ST_INT64);
                        args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                            {
                                arg = nullptr;
                                m_args_value.read_value(tr, e, arg);
                                if (!tstr.empty())
                                {
                                    tstr += " ";
                                }
                                if (arg)
                                {
                                    tstr += arg;
                                }
                                return true;
                            });
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXELINE:
        {
            m_exe.read_value(tr, thread_entry, tstr);
            const char* arg = nullptr;
            auto args_table = m_thread_table.get_subtable(tr, m_args, thread_entry, st::SS_PLUGIN_ST_INT64);
            args_table.iterate_entries(tr, [this, &tr, &arg, &tstr](const falcosecurity::table_entry& e)
                {
                    arg = nullptr;
                    m_args_value.read_value(tr, e, arg);
                    if (!tstr.empty())
                    {
                        tstr += " ";
                    }
                    if (arg)
                    {
                        tstr += arg;
                    }
                    return true;
                });
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXE:
            m_exe.read_value(tr, thread_entry, tstr);
            break;
        case plugin_sinsp_filterchecks::TYPE_PEXE:
        {
            m_ptid.read_value(tr, thread_entry, ptid);
            auto lineage = m_thread_table.get_entry(tr, ptid);
            m_exe.read_value(tr, lineage, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_AEXE:
        {
            if(field.argid < 1)
            {
                m_exe.read_value(tr, thread_entry, tstr);
                break;
            }
            m_ptid.read_value(tr, thread_entry, ptid);
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    if(j == (field.argid - 1))
                    {
                        m_exe.read_value(tr, lineage, tstr);
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXEPATH:
            m_exepath.read_value(tr, thread_entry, tstr);
            break;
        case plugin_sinsp_filterchecks::TYPE_PEXEPATH:
        {
            m_ptid.read_value(tr, thread_entry, ptid);
            auto lineage = m_thread_table.get_entry(tr, ptid);
            m_exepath.read_value(tr, lineage, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_AEXEPATH:
        {
            if(field.argid < 1)
            {
                m_exepath.read_value(tr, thread_entry, tstr);
                break;
            }
            m_ptid.read_value(tr, thread_entry, ptid);
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    if(j == (field.argid - 1))
                    {
                        m_exepath.read_value(tr, lineage, tstr);
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CWD:
            m_cwd.read_value(tr, thread_entry, tstr);
            break;
        case plugin_sinsp_filterchecks::TYPE_TTY:
            m_tty.read_value(tr, thread_entry, tuint32);
            tstr = std::to_string(tuint32);
            break;
        case plugin_sinsp_filterchecks::TYPE_PID:
            m_pid.read_value(tr, thread_entry, tint64);
            tstr = std::to_string(tint64);
            break;
        case plugin_sinsp_filterchecks::TYPE_PPID:
            {
                m_ptid.read_value(tr, thread_entry, ptid);
                auto lineage = m_thread_table.get_entry(tr, ptid);
                m_pid.read_value(tr, lineage, tint64);
                tstr = std::to_string(tint64);
                break;
            }
        case plugin_sinsp_filterchecks::TYPE_APID:
        {
            if(field.argid < 1)
            {
                m_pid.read_value(tr, thread_entry, tint64);
                tstr = std::to_string(tint64);
                break;
            }
            m_ptid.read_value(tr, thread_entry, ptid);
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    if(j == (field.argid - 1))
                    {
                        m_pid.read_value(tr, lineage, tint64);
                        tstr = std::to_string(tint64);
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_VPID:
            m_vpid.read_value(tr, thread_entry, tint64);
            tstr = std::to_string(tint64);
            break;
        case plugin_sinsp_filterchecks::TYPE_PVPID:
            {
                m_ptid.read_value(tr, thread_entry, ptid);
                auto lineage = m_thread_table.get_entry(tr, ptid);
                m_vpid.read_value(tr, lineage, tint64);
                tstr = std::to_string(tint64);
                break;
            }
        case plugin_sinsp_filterchecks::TYPE_SID:
            m_sid.read_value(tr, thread_entry, tint64);
            tstr = std::to_string(tint64);
            break;
        case plugin_sinsp_filterchecks::TYPE_SNAME:
        {
            int64_t sid;
            m_sid.read_value(tr, thread_entry, sid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 9; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_sid.read_value(tr, lineage, tint64);
                    if(sid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_comm.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_SID_EXE:
        {
            int64_t sid;
            m_sid.read_value(tr, thread_entry, sid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 9; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_sid.read_value(tr, lineage, tint64);
                    if(sid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_exe.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_SID_EXEPATH:
        {
            int64_t sid;
            m_sid.read_value(tr, thread_entry, sid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 9; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_sid.read_value(tr, lineage, tint64);
                    if(sid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_exepath.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_VPGID:
            m_vpgid.read_value(tr, thread_entry, tint64);
            tstr = std::to_string(tint64);
            break;
        case plugin_sinsp_filterchecks::TYPE_VPGID_NAME:
        {
            int64_t vpgid;
            m_vpgid.read_value(tr, thread_entry, vpgid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 5; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_vpgid.read_value(tr, lineage, tint64);
                    if(vpgid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_comm.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_VPGID_EXE:
        {
            int64_t vpgid;
            m_vpgid.read_value(tr, thread_entry, vpgid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 5; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_vpgid.read_value(tr, lineage, tint64);
                    if(vpgid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_exe.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_VPGID_EXEPATH:
        {
            int64_t vpgid;
            m_vpgid.read_value(tr, thread_entry, vpgid);
            m_ptid.read_value(tr, thread_entry, ptid);
            falcosecurity::table_entry last_entry(nullptr, nullptr, nullptr);
            falcosecurity::table_entry* leader = &thread_entry;
            for(uint32_t j = 0; j < 5; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_vpgid.read_value(tr, lineage, tint64);
                    if(vpgid != tint64)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                    last_entry = std::move(lineage);
                    leader = &last_entry;
                }
                catch(const std::exception& e)
                {
                }
            }
            m_exepath.read_value(tr, *leader, tstr);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_ENV:
        {
            const char* env = nullptr;
            auto env_table = m_thread_table.get_subtable(tr, m_env, thread_entry, st::SS_PLUGIN_ST_INT64);
            auto argname = field.argname;
            if(!argname.empty())
            {
                size_t nlen = argname.length();
                env_table.iterate_entries(tr, [this, &tr, &env, &tstr, &nlen, &argname](const falcosecurity::table_entry& e)
                {
                    env = nullptr;
                    std::string env_var;
                    m_env_value.read_value(tr, e, env);
                    if (env != nullptr)
                    {
                        env_var = std::string(env);
                    }
                    if((env_var.length() > (nlen + 1)) && (env_var[nlen] == '=') &&
			!env_var.compare(0, nlen, argname))
                    {
                        size_t first = env_var.find_first_not_of(' ', nlen + 1);
                        size_t last = env_var.find_last_not_of(' ');
                        tstr = env_var.substr(first, last - first + 1);
                    }
                    return true;
                });
            } else
            {
                env_table.iterate_entries(tr, [this, &tr, &env, &tstr](const falcosecurity::table_entry& e)
                    {
                        env = nullptr;
                        m_env_value.read_value(tr, e, env);
                        if (!tstr.empty())
                        {
                            tstr += " ";
                        }
                        if (env)
                        {
                            tstr += env;
                        }
                        return true;
                    });
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_IS_EXE_WRITABLE:
        {
            m_exe_writable.read_value(tr, thread_entry, tbool);
            tstr = std::to_string(tbool);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_IS_EXE_UPPER_LAYER:
        {
            m_exe_upper_layer.read_value(tr, thread_entry, tbool);
            tstr = std::to_string(tbool);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_IS_EXE_FROM_MEMFD:
        {
            m_exe_from_memfd.read_value(tr, thread_entry, tbool);
            tstr = std::to_string(tbool);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXE_INO:
        {
            m_exe_ino.read_value(tr, thread_entry, tuint64);
            tstr = std::to_string(tuint64);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXE_INO_CTIME:
        {
            m_exe_ino_ctime.read_value(tr, thread_entry, tuint64);
            tstr = std::to_string(tuint64);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_EXE_INO_MTIME:
        {
            m_exe_ino_mtime.read_value(tr, thread_entry, tuint64);
            tstr = std::to_string(tuint64);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_IS_SID_LEADER:
        {
            uint64_t vpid;
            m_sid.read_value(tr, thread_entry, tint64);
            m_vpid.read_value(tr, thread_entry, vpid);
            tbool = tint64 == vpid;
            tstr = std::to_string(tbool);
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_IS_VPGID_LEADER:
        {
            uint64_t vpid;
            m_vpgid.read_value(tr, thread_entry, tint64);
            m_vpid.read_value(tr, thread_entry, vpid);
            tbool = tint64 == vpid;
            tstr = std::to_string(tbool);
            break;
        }


        //
        // fd related
        //

        case plugin_sinsp_filterchecks::TYPE_FDNUM:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SYSCALL_CREAT_X:
            case PPME_SOCKET_CONNECT_X:
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                tstr = std::to_string(tint64);
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_FDNAME:
        {
            switch(evt.get_type())
            {
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                break;
            }
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    std::string cwd;
                    m_cwd.read_value(tr, thread_entry, cwd);
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field, cwd);
                }
                break;
            }
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                1);
                    std::string cwd;
                    if (res_param.param_pointer != nullptr)
                    {
                        int64_t dirfd = *(uint64_t*)(res_param.param_pointer);
                        try
                        {
                            auto fd_table = m_thread_table.get_subtable(
                            tr, m_fds, thread_entry,
                            st::SS_PLUGIN_ST_INT64);
                            auto fd_entry = fd_table.get_entry(tr, dirfd);
                            if (dirfd == PPM_AT_FDCWD)
                            {
                                m_cwd.read_value(tr, thread_entry, cwd);

                            } else
                            {
                                m_fd_name_value.read_value(tr, fd_entry, cwd);
                            }
                        }
                        catch(const std::exception& e)
                        {
                        }
                    }
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field, cwd);
                }
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_DIRECTORY:
        case plugin_sinsp_filterchecks::TYPE_FILENAME:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    std::string cwd;
                    m_cwd.read_value(tr, thread_entry, cwd);
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field, cwd);
                }
                size_t pos = tstr.find_last_of('/');
                if (pos != std::string::npos)
                {
                    if (field.id == plugin_sinsp_filterchecks::TYPE_DIRECTORY)
                    {
                        tstr = tstr.substr(0, pos);
                    } else
                    {

                        tstr = tstr.substr(pos + 1);
                    }
                }
                break;
            }
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                size_t pos = tstr.find_last_of('/');
                if (pos != std::string::npos)
                {
                    if (field.id == plugin_sinsp_filterchecks::TYPE_DIRECTORY)
                    {
                        tstr = tstr.substr(0, pos);
                    } else
                    {

                        tstr = tstr.substr(pos + 1);
                    }
                }
                break;
            }
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    auto res_param = get_syscall_evt_param(evt.get_buf(),
                                                1);
                    std::string cwd;
                    if (res_param.param_pointer != nullptr)
                    {
                        int64_t dirfd = *(uint64_t*)(res_param.param_pointer);
                        try
                        {
                            auto fd_table = m_thread_table.get_subtable(
                            tr, m_fds, thread_entry,
                            st::SS_PLUGIN_ST_INT64);
                            auto fd_entry = fd_table.get_entry(tr, dirfd);
                            if (dirfd == PPM_AT_FDCWD)
                            {
                                m_cwd.read_value(tr, thread_entry, cwd);

                            } else
                            {
                                m_fd_name_value.read_value(tr, fd_entry, cwd);
                            }
                        }
                        catch(const std::exception& e)
                        {
                        }
                    }
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field, cwd);
                }
                size_t pos = tstr.find_last_of('/');
                if (pos != std::string::npos)
                {
                    if (field.id == plugin_sinsp_filterchecks::TYPE_DIRECTORY)
                    {
                        tstr = tstr.substr(0, pos);
                    } else
                    {

                        tstr = tstr.substr(pos + 1);
                    }
                }
                break;
            }
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            {
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_INO:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_ino_value.read_value(tr, fd_entry, tint64);
                    tstr = std::to_string(tint64);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                break;
            }
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            {
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_DEV:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_dev_value.read_value(tr, fd_entry, tuint32);
                    tstr = std::to_string(tuint32);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                break;
            }
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            {
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_FDNAMERAW:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_nameraw_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                break;
            }
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            {
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }

        //
        // Custom behavior profile short-cut fields
        //

        case plugin_sinsp_filterchecks::TYPE_CUSTOM_ANAME_LINEAGE_CONCAT:
        {
            if(field.argid < 1)
            {
                break;
            }
            m_comm.read_value(tr, thread_entry, tstr);
            m_ptid.read_value(tr, thread_entry, ptid);
            std::string tstr2;
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_comm.read_value(tr, lineage, tstr2);
                    tstr += tstr2;
                    tstr2.clear();
                    if(j == (field.argid - 1))
                    {
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CUSTOM_AEXE_LINEAGE_CONCAT:
        {
            if(field.argid < 1)
            {
                break;
            }
            m_exe.read_value(tr, thread_entry, tstr);
            m_ptid.read_value(tr, thread_entry, ptid);
            std::string tstr2;
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_exe.read_value(tr, lineage, tstr2);
                    tstr += tstr2;
                    tstr2.clear();
                    if(j == (field.argid - 1))
                    {
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CUSTOM_AEXEPATH_LINEAGE_CONCAT:
        {
            if(field.argid < 1)
            {
                break;
            }
            m_exepath.read_value(tr, thread_entry, tstr);
            m_ptid.read_value(tr, thread_entry, ptid);
            std::string tstr2;
            for(uint32_t j = 0; j < field.argid; j++)
            {
                try
                {
                    auto lineage = m_thread_table.get_entry(tr, ptid);
                    m_exepath.read_value(tr, lineage, tstr2);
                    tstr += tstr2;
                    tstr2.clear();
                    if(j == (field.argid - 1))
                    {
                        break;
                    }
                    if(ptid == 1)
                    {
                        break;
                    }
                    m_ptid.read_value(tr, lineage, ptid);
                }
                catch(const std::exception& e)
                {
                }
            }
            break;
        }
        case plugin_sinsp_filterchecks::TYPE_CUSTOM_FDNAME_PART1:
        case plugin_sinsp_filterchecks::TYPE_CUSTOM_FDNAME_PART2:
        {
            switch(evt.get_type())
            {
            case PPME_SYSCALL_OPEN_X:
            case PPME_SYSCALL_CREAT_X:
            case PPME_SYSCALL_OPENAT_2_X:
            case PPME_SYSCALL_OPENAT2_X:
            case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
            {
                break;
            }
            case PPME_SOCKET_ACCEPT_5_X:
            case PPME_SOCKET_ACCEPT4_6_X:
            case PPME_SOCKET_CONNECT_X:
            {
                try
                {
                    auto fd_table = m_thread_table.get_subtable(
                    tr, m_fds, thread_entry,
                    st::SS_PLUGIN_ST_INT64);
                    m_lastevent_fd_field.read_value(tr, thread_entry, tint64);
                    auto fd_entry = fd_table.get_entry(tr, tint64);
                    m_fd_name_value.read_value(tr, fd_entry, tstr);
                }
                catch(const std::exception& e)
                {
                }
                if (tstr.empty())
                {
                    tstr = extract_filterchecks_evt_params_fallbacks(evt, field);
                }
                std::string delimiter = "->";
                size_t pos = tstr.find(delimiter);
                if (pos != std::string::npos) 
                {
                    if (field.id == plugin_sinsp_filterchecks::TYPE_CUSTOM_FDNAME_PART1)
                    {
                        tstr = tstr.substr(0, pos);

                    } else
                    {
                        tstr = tstr.substr(pos + delimiter.length());
                    }
                } else
                {
                    tstr.clear();
                }
                break;
            }
            default:
                // Clear the entire profile when invoking the fd related profile for non fd syscalls
                behavior_profile_concat_str.clear();
                break;
            }
            break;
        }

        default:
            break;
        }
        behavior_profile_concat_str += tstr;
    }
    return true;
}

bool anomalydetection::parse_event(const falcosecurity::parse_event_input& in)
{
    /* Note: While we have set the stage for supporting multiple algorithms in this plugin, 
       the approach is currently designed specific to the count_min_sketch use case only. 
       This will be refactored and refined in the future.
    */
    if (!m_count_min_sketch_enabled)
    {
        return true;
    }

    auto& evt = in.get_event_reader();
    auto& tr = in.get_table_reader();
    auto& tw = in.get_table_writer();
    int64_t thread_id = evt.get_tid();

    // Note: Plugin event parsing guaranteed to happen after libs' `sinsp_parser::process_event` has finished.
    // Needs to stay in sync w/ falcosecurity/libs updates.

    // The plugin currently cannot access for examle m_lastevent_fd from falcosecurity/libs
    // Write this info to tinfo within the plugin
    switch(evt.get_type())
    {
    case PPME_SYSCALL_OPEN_X: // fd param 0
    case PPME_SOCKET_ACCEPT_5_X:
    case PPME_SOCKET_ACCEPT4_6_X:
    case PPME_SYSCALL_CREAT_X:
    case PPME_SYSCALL_OPENAT_2_X:
    case PPME_SYSCALL_OPENAT2_X:
    case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
    {
        auto res_param = get_syscall_evt_param(in.get_event_reader().get_buf(),
                                           0);
        if (res_param.param_pointer == nullptr)
        {
            return false;
        }

        int64_t fd = *(int64_t*)(res_param.param_pointer);
        auto thread_entry = m_thread_table.get_entry(tr, thread_id);
        m_lastevent_fd_field.write_value(tw, thread_entry, fd);
        break;
    }
    case PPME_SOCKET_CONNECT_X: // fd param 2
    {
        auto res_param = get_syscall_evt_param(in.get_event_reader().get_buf(),
                                           2);
        if (res_param.param_pointer == nullptr)
        {
            return false;
        }
        int64_t fd = *(int64_t*)(res_param.param_pointer);
        auto thread_entry = m_thread_table.get_entry(tr, thread_id);
        m_lastevent_fd_field.write_value(tw, thread_entry, fd);
        break;
    }
    default:
        break;
    }

    // Loop over behavior profiles, extract profile fields and update the count_min_sketch counts.
    int i = 0;
    std::string behavior_profile_concat_str;
    for(const auto& set : m_behavior_profiles_event_codes)
    {
        if(set.find((ppm_event_code)evt.get_type()) != set.end())
        {
            if(thread_id <= 0)
            {
                return false;
            }
            try
            {
                behavior_profile_concat_str.clear();
                if (i < m_n_sketches && extract_filterchecks_concat_profile(evt, tr, m_behavior_profiles_fields[i], behavior_profile_concat_str) && !behavior_profile_concat_str.empty())
                {
                    m_count_min_sketches.lock()->at(i).get()->update(behavior_profile_concat_str, (uint64_t)1);
                }
            }
            catch(falcosecurity::plugin_exception e)
            {
                return false;
            }
        }
        i++;
    }
    return true;
}
