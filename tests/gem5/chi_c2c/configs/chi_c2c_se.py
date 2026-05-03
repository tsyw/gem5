# Copyright (c) 2025 The gem5 Contributors
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
SE-mode CHI C2C config for AArch64 benchmark experiments.

Supports R1/R2/R3 topology variants without requiring a kernel or disk image.

Usage:
    gem5.debug chi_c2c_se.py \
            --topology R2 --num-cores 64 \
            --mesh-rows 8 --num-c2cgs 4 \
            --bin /path/to/stream_triad --args 64 4194304 \
            [--link-bw-gbps 256] [--txq-size 256] [--stream-data-pool 1]
"""

import argparse
import importlib
import math
import os
import sys
import time

import m5
from m5.objects import *
from m5.util import addToPath

m5_simulate = importlib.import_module("m5.simulate")


# ------------------------------------------------------------------
# Path setup — works for both in-tree and EXTRAS builds
# ------------------------------------------------------------------
def _resolve_configs_dir():
    candidates = [os.path.join(m5.util.repoPath(), "configs")]

    _src = os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", "")
    if _src:
        candidates.append(os.path.join(_src, "configs"))

    candidates.append(
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..",
            "..",
            "..",
            "..",
            "configs",
        )
    )

    for candidate in candidates:
        candidate = os.path.realpath(candidate)
        if os.path.isdir(os.path.join(candidate, "common")):
            return candidate

    raise ImportError("Unable to locate gem5 configs directory")


_configs = _resolve_configs_dir()
addToPath(_configs)
addToPath(os.path.join(_configs, "ruby"))

from common import (
    ObjectList,
    Options,
)
from ruby import Ruby
from ruby.CHI_config import NoC_Params

try:
    _here = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _here = os.path.join(
        os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", m5.util.repoPath()),
        "tests",
        "gem5",
        "chi_c2c",
        "configs",
    )
sys.path.insert(0, _here)

from chi_c2c_four_chip import build_four_chip_system
from chi_c2c_fs import (
    _SVE_VL,
    _build_r1,
    _make_hbm2_ctrl,
)
from chi_c2c_two_chip import build_two_chip_system
from CHI_Mesh_XY import CHI_Mesh_XY
from CHI_Mesh_XY import add_options as _add_mesh_options

_STREAM_DEFAULT_N = 4194304
_STREAM_DEFAULT_BASE_VADDR = 0x7000000000
_STREAM_PAGE_BYTES = 4096


def _split_addr_range(range_obj, num_parts):
    total = int(range_obj.size())
    part_size = total // num_parts
    start = int(range_obj.start)
    parts = []
    for i in range(num_parts):
        p_start = start + i * part_size
        p_size = part_size if i < num_parts - 1 else (total - i * part_size)
        parts.append(AddrRange(start=p_start, size=p_size))
    return parts


def _is_stream_triad(bin_path):
    return os.path.basename(bin_path) == "stream_triad"


def _prepare_process_args(
    bin_path, raw_args, stream_data_pool, stream_base_vaddr
):
    process_args = list(raw_args or [])
    if _is_stream_triad(bin_path) and stream_data_pool >= 0:
        if len(process_args) < 2:
            process_args.append(str(_STREAM_DEFAULT_N))
        process_args.append(hex(stream_base_vaddr))
    return process_args


def _stream_array_elems(process_args):
    if len(process_args) >= 2:
        return int(process_args[1], 0)
    return _STREAM_DEFAULT_N


def _map_stream_arrays(
    system, process, process_args, stream_data_pool, stream_base_vaddr
):
    if stream_data_pool < 0 or not _is_stream_triad(process.executable):
        return

    if stream_data_pool >= len(system.mem_ranges):
        m5.fatal(
            f"stream-data-pool={stream_data_pool} is out of range for "
            f"{len(system.mem_ranges)} memory pools"
        )

    array_bytes = _stream_array_elems(process_args) * 8
    mapped_bytes = 3 * array_bytes
    total_pages = (mapped_bytes + _STREAM_PAGE_BYTES - 1) // _STREAM_PAGE_BYTES
    mapped_bytes = total_pages * _STREAM_PAGE_BYTES
    paddr = system.workload.allocPhysPages(total_pages, stream_data_pool)
    process.map(stream_base_vaddr, paddr, mapped_bytes, True)
    print(
        "Mapped STREAM arrays "
        f"pool={stream_data_pool} "
        f"vaddr={stream_base_vaddr:#x} "
        f"paddr={paddr:#x} bytes={mapped_bytes}",
        flush=True,
    )


def _configure_sve(cpu):
    for isa in getattr(cpu, "isa", []):
        if hasattr(isa, "sve_vl_se"):
            isa.sve_vl_se = _SVE_VL


def _trace_phase(label, action):
    start = time.monotonic()
    print(f"[instantiate] begin {label}", flush=True)
    action()
    elapsed = time.monotonic() - start
    print(f"[instantiate] done {label} in {elapsed:.2f}s", flush=True)


def _trace_object_phase(label, objects, method_name, progress_step=25):
    total = len(objects)
    start = time.monotonic()
    print(f"[instantiate] begin {label} ({total} objects)", flush=True)

    for index, obj in enumerate(objects, start=1):
        getattr(obj, method_name)()
        if index % progress_step == 0 or index == total:
            print(
                f"[instantiate] {label}: {index}/{total} "
                f"last={obj.path()}",
                flush=True,
            )

    elapsed = time.monotonic() - start
    print(f"[instantiate] done {label} in {elapsed:.2f}s", flush=True)


def _instantiate_with_phase_trace(root):
    if m5_simulate._instantiated:
        raise RuntimeError("m5.instantiate() called twice.")

    m5_simulate._instantiated = True

    _trace_phase("fix_all_objects", lambda: m5_simulate._fix_all_objects(root))
    _trace_phase(
        "dump_configs_pre_cpp", lambda: m5_simulate._dump_configs(root)
    )

    objects = list(root.descendants())
    print(f"[instantiate] descendant_count={len(objects)}", flush=True)

    _trace_object_phase("createCCObject", objects, "createCCObject")
    _trace_object_phase("connectPorts", objects, "connectPorts")
    _trace_object_phase("init", objects, "init")

    _trace_phase(
        "bindStatHierarchy",
        lambda: m5_simulate.stats._bindStatHierarchy(root),
    )
    _trace_phase("regStats", root.regStats)

    _trace_object_phase("regProbePoints", objects, "regProbePoints")
    _trace_object_phase("regProbeListeners", objects, "regProbeListeners")

    _trace_phase("stats.enable", m5_simulate.stats.enable)

    _trace_object_phase("initState", objects, "initState")
    _trace_phase("updateStatEvents", m5_simulate.updateStatEvents)
    _trace_phase(
        "dump_configs_post_cpp",
        lambda: m5_simulate._dump_configs_post_cpp(root),
    )


# ------------------------------------------------------------------
# Argument parsing
# ------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="CHI C2C SE-mode benchmark config",
    conflict_handler="resolve",
)
Options.addNoISAOptions(parser)
Ruby.define_options(parser)

parser.add_argument(
    "--topology",
    default="R2",
    choices=["R1", "R2", "R3"],
    help="Chiplet topology: R1 (monolithic), R2 (2-chip), or R3 (4-chip)",
)
parser.add_argument(
    "--num-cores",
    type=int,
    default=64,
    help="Total CPU cores across all chips (default 64)",
)
parser.add_argument(
    "--num-hnfs",
    type=int,
    default=4,
    help=(
        "HNF/SNF count for R1 NUMA domains. "
        "Must be a power of 2 (default 4)"
    ),
)
parser.add_argument(
    "--num-c2cgs",
    type=int,
    default=2,
    help="C2CGs per chip: R2 uses 2/4, R3 uses 3/6 (default 2)",
)
parser.add_argument(
    "--mesh-rows",
    type=int,
    default=8,
    metavar="R",
    help="Rows in CHI_Mesh_XY topology (default 8)",
)
parser.add_argument(
    "--link-bw-gbps",
    type=float,
    default=0.0,
    metavar="BW",
    help=(
        "C2C link bandwidth in GB/s; overrides --container-latency. "
        "0 = use --container-latency directly."
    ),
)
parser.add_argument(
    "--container-latency",
    type=int,
    default=1,
    metavar="CYCLES",
    help="C2C container latency in cycles (default 1)",
)
parser.add_argument(
    "--txq-size",
    type=int,
    default=0,
    metavar="N",
    help="C2C TX queue size in granules (0=unlimited)",
)
parser.add_argument(
    "--num-tbes",
    type=int,
    default=32,
    metavar="N",
    help="Number of C2CG transaction TBEs per gateway (paper sweeps 256/512).",
)
parser.add_argument(
    "--stream-data-pool",
    type=int,
    default=-1,
    metavar="POOL",
    help=(
        "Explicitly map stream_triad arrays into the selected SE memory pool "
        "after instantiate (-1 disables explicit mapping)."
    ),
)
parser.add_argument(
    "--stream-base-vaddr",
    type=lambda value: int(value, 0),
    default=_STREAM_DEFAULT_BASE_VADDR,
    metavar="ADDR",
    help="Base virtual address used for explicit stream_triad array mapping.",
)
parser.add_argument(
    "--mem-size",
    default="8GiB",
    help="Total simulated memory size (default 8GiB)",
)
parser.add_argument(
    "--bin",
    required=True,
    metavar="PATH",
    help="Path to AArch64 binary to run",
)
parser.add_argument(
    "--args",
    nargs="*",
    default=[],
    metavar="ARG",
    help="Extra arguments to pass to the binary",
)
parser.add_argument(
    "--cpu-type",
    default="Timing",
    choices=["Timing", "O3"],
    help="CPU model: Timing (faster) or O3 (default Timing)",
)
parser.add_argument(
    "--max-ticks",
    type=int,
    default=10**12,
    metavar="T",
    help="Simulation stop time in ticks (default 10^12)",
)
parser.add_argument(
    "--cacheline-size",
    type=int,
    default=64,
    help="Cache line size in bytes (default 64)",
)
parser.add_argument(
    "--phase-trace-instantiate",
    action="store_true",
    help="Trace internal m5.instantiate() phases for debugging startup stalls",
)

args = parser.parse_args()

# Inherit cache parameters expected by build_two_chip_system
if not hasattr(args, "l1i_size"):
    args.l1i_size = "64KiB"
if not hasattr(args, "l1i_assoc"):
    args.l1i_assoc = 4
if not hasattr(args, "l1d_size"):
    args.l1d_size = "64KiB"
if not hasattr(args, "l1d_assoc"):
    args.l1d_assoc = 4
if not hasattr(args, "l2_size"):
    args.l2_size = "512KiB"
if not hasattr(args, "l2_assoc"):
    args.l2_assoc = 8

# Convert bandwidth to container latency (1 GHz reference clock)
if args.link_bw_gbps > 0.0:
    args.container_latency = max(1, math.ceil(256.0 / args.link_bw_gbps))
    print(
        f"link-bw-gbps={args.link_bw_gbps:.1f} → "
        f"container_latency={args.container_latency} cycles"
    )

num_cores = args.num_cores
topology = args.topology.upper()

# ------------------------------------------------------------------
# System
# ------------------------------------------------------------------
full_range = AddrRange(args.mem_size)

if topology == "R1":
    mem_ranges = _split_addr_range(full_range, args.num_hnfs)
elif topology == "R2":
    half = full_range.size() // 2
    chip0_range = AddrRange(start=0, size=half)
    chip1_range = AddrRange(start=half, size=half)
    mem_ranges = [chip0_range, chip1_range]
else:
    chip_ranges = _split_addr_range(full_range, 4)
    mem_ranges = chip_ranges

system = System(mem_ranges=mem_ranges)
system.workload = SEWorkload.init_compatible(args.bin)
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock,
    voltage_domain=system.voltage_domain,
)
system.mem_mode = "timing"
system.cache_line_size = args.cacheline_size

# ------------------------------------------------------------------
# CPUs
# ------------------------------------------------------------------
process_args = _prepare_process_args(
    args.bin,
    args.args,
    args.stream_data_pool,
    args.stream_base_vaddr,
)

shared_process = Process(
    pid=100,
    executable=args.bin,
    cmd=[args.bin] + process_args,
)

cpus = []
for i in range(num_cores):
    if args.cpu_type == "O3":
        cpu = ArmO3CPU(cpu_id=i)
    else:
        cpu = ArmTimingSimpleCPU(cpu_id=i)
    _configure_sve(cpu)
    cpu.createThreads()
    cpu.workload = shared_process
    cpus.append(cpu)
system.cpu = cpus

for cpu in cpus:
    cpu.createInterruptController()

# ------------------------------------------------------------------
# Ruby
# ------------------------------------------------------------------
system.ruby = RubySystem()
system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock,
    voltage_domain=system.voltage_domain,
)
system.ruby.network = SimpleNetwork(
    ruby_system=system.ruby,
    routers=[],
    ext_links=[],
    int_links=[],
    netifs=[],
)

# ------------------------------------------------------------------
# CHI hierarchy
# ------------------------------------------------------------------
if topology == "R1":
    (
        net_nodes,
        net_cntrls,
        all_cntrls,
        cpu_seqs,
        snf_cntrls,
        numa_ranges,
    ) = _build_r1(
        system.ruby,
        args.cacheline_size,
        cpus,
        full_range,
        num_hnfs=args.num_hnfs,
    )
    snf_mem_ranges = numa_ranges

elif topology == "R2":
    (
        net_nodes,
        net_cntrls,
        all_cntrls,
        cpu_seqs,
        snf_cntrls,
        _,
    ) = build_two_chip_system(
        system.ruby,
        system,
        args,
        cpus,
        chip0_range=chip0_range,
        chip1_range=chip1_range,
        container_latency=args.container_latency,
        num_c2cgs=args.num_c2cgs,
        txq_size=args.txq_size,
        num_tbes=args.num_tbes,
    )
    snf_mem_ranges = [chip0_range, chip1_range]

else:  # R3
    (
        net_nodes,
        net_cntrls,
        all_cntrls,
        cpu_seqs,
        snf_cntrls,
        _,
    ) = build_four_chip_system(
        system.ruby,
        system,
        args,
        cpus,
        chip_ranges=chip_ranges,
        container_latency=args.container_latency,
        num_c2cgs=args.num_c2cgs,
        txq_size=args.txq_size,
        num_tbes=args.num_tbes,
    )
    snf_mem_ranges = chip_ranges

# ------------------------------------------------------------------
# Topology — CHI_Mesh_XY
# ------------------------------------------------------------------
args.num_cpus = len(net_cntrls)

topo = CHI_Mesh_XY(net_cntrls)
topo.makeTopology(
    args,
    system.ruby.network,
    SimpleIntLink,
    SimpleExtLink,
    Switch,
)
system.ruby.network.number_of_virtual_networks = 4
system.ruby.network.control_msg_size = NoC_Params.cntrl_msg_size
system.ruby.network.data_msg_size = NoC_Params.data_width
if getattr(args, "network", "simple") == "simple":
    system.ruby.network.buffer_size = NoC_Params.router_buffer_size
system.ruby.network.setup_buffers()

# ------------------------------------------------------------------
# Ruby system parameters
# ------------------------------------------------------------------
system.ruby.block_size_bytes = args.cacheline_size
system.ruby.memory_size_bits = 48
system.ruby.number_of_virtual_networks = (
    system.ruby.network.number_of_virtual_networks
)
system.ruby._cpu_ports = cpu_seqs
system.ruby.num_of_sequencers = len(cpu_seqs)

assert (
    len(cpu_seqs) == num_cores
), f"Expected {num_cores} sequencers, got {len(cpu_seqs)}"

# ------------------------------------------------------------------
# Wire sequencers to CPUs
# ------------------------------------------------------------------
for cpu, seq in zip(cpus, cpu_seqs):
    cpu.icache_port = seq.in_ports
    cpu.dcache_port = seq.in_ports

# ------------------------------------------------------------------
# HBM2 memory controllers
# ------------------------------------------------------------------
for i, (snf_cntrl, mem_range) in enumerate(zip(snf_cntrls, snf_mem_ranges)):
    ctrl = _make_hbm2_ctrl(mem_range)
    ctrl.port = snf_cntrl.memory_out_port
    snf_cntrl.addr_ranges = [mem_range]
    setattr(system, f"mem_ctrl{i}", ctrl)

# ------------------------------------------------------------------
# System port proxy
# ------------------------------------------------------------------
sys_port_proxy = RubyPortProxy(ruby_system=system.ruby)
system.sys_port_proxy = sys_port_proxy
system.system_port = system.sys_port_proxy.in_ports

# ------------------------------------------------------------------
# Root and simulate
# ------------------------------------------------------------------
root = Root(full_system=False, system=system)
m5.ticks.setGlobalFrequency("1ns")
if args.phase_trace_instantiate:
    _instantiate_with_phase_trace(root)
else:
    m5.instantiate()

_map_stream_arrays(
    system,
    shared_process,
    process_args,
    args.stream_data_pool,
    args.stream_base_vaddr,
)

print(
    f"Starting {topology} SE simulation, "
    f"{num_cores} cores, cpu-type={args.cpu_type}"
)
exit_event = m5.simulate(args.max_ticks)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
