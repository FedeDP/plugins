// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2025 The Falco Authors.

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

use aya_ebpf::{
    helpers::bpf_get_smp_processor_id,
    macros::map,
    maps::{Array, RingBuf},
};
use krsi_common::flags::{FeatureFlags, OpFlags};

pub mod op_info;

#[map]
// The number of max entries is set, in userspace, to the value of available CPU.
static AUXILIARY_BUFFERS: Array<crate::auxbuf::AuxiliaryBuffer> = Array::with_max_entries(0, 0);

#[no_mangle]
static BOOT_TIME: u64 = 0;

#[no_mangle]
static FEATURE_FLAGS: u8 = 0;
#[no_mangle]
static OP_FLAGS: u64 = 0;

#[map]
static EVENTS: RingBuf = RingBuf::with_byte_size(128 * 4096, 0); // 128 pages = 256KB

pub fn auxiliary_buffer() -> Option<&'static mut crate::auxbuf::AuxiliaryBuffer> {
    let cpu_id = unsafe { bpf_get_smp_processor_id() };
    AUXILIARY_BUFFERS
        .get_ptr_mut(cpu_id)
        .map(|p| unsafe { &mut *p })
}

pub fn events_ringbuf() -> &'static RingBuf {
    &EVENTS
}

pub fn boot_time() -> u64 {
    unsafe { core::ptr::read_volatile(&BOOT_TIME) }
}

fn enabled_feature_flags() -> FeatureFlags {
    FeatureFlags::from_bits_truncate(unsafe { core::ptr::read_volatile(&FEATURE_FLAGS) })
}

fn enabled_op_flags() -> OpFlags {
    OpFlags::from_bits_truncate(unsafe { core::ptr::read_volatile(&OP_FLAGS) })
}

pub fn is_support_enabled(feature_flags: FeatureFlags, op_flags: OpFlags) -> bool {
    let enabled_op_flags = enabled_op_flags();
    let enabled_feature_flags = enabled_feature_flags();
    enabled_feature_flags.contains(feature_flags) && enabled_op_flags.contains(op_flags)
}
