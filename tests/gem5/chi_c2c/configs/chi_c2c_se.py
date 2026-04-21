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

Supports the same R1/R2 topology variants as chi_c2c_fs.py but uses
syscall emulation (no disk image, no kernel required).

Usage:
  gem5.debug chi_c2c_se.py \\
      --topology R1 --num-cores 64 \\
      --num-hnfs 4 --mesh-rows 8 \\
      --benchmark stream --bin /path/to/stream \\
      [--num-c2cgs 2] [--link-bw-gbps 256] [--txq-size 256]
"""

import argparse
import math
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# ------------------------------------------------------------------
# Path setup — works for both in-tree and EXTRAS builds
# ------------------------------------------------------------------
_configs = os.path.join(m5.util.repoPath(), "configs")
if not os.path.isdir(os.path.join(_configs, "common")):
    _src = os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", "")
    if _src:
        _configs = os.path.join(_src, "configs")
addToPath(_configs)
addToPath(os.path.join(_configs, "ruby"))

from common import (
    ObjectList,
    Options,
)
from ruby import Ruby

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

from chi_c2c_fs import (
    _build_r1,
    _make_hbm2_ctrl,
    _PaperL1D,
    _PaperL1I,
    _PaperL2,
    _PaperO3CPU,
    _PaperSLC,
)
from chi_c2c_two_chip import build_two_chip_system
from CHI_Mesh_XY import CHI_Mesh_XY
from CHI_Mesh_XY import add_options as _add_mesh_options

# ------------------------------------------------------------------
# Argument parsing
# ------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="CHI C2C SE-mode benchmark config"
)
Options.addNoISAOptions(parser)
Ruby.define_options(parser)

parser.add_argument(
    "--topology",
    default="R2",
    choices=["R1", "R2"],
    help="Chiplet topology: R1 (monolithic) or R2 (2-chip C2C)",
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
    help="C2CGs per chip for R2 topology (default 2)",
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
full_range = AddrRange(size=args.mem_size)

if topology == "R1":
    mem_ranges = [full_range]
else:
    half = full_range.size() // 2
    chip0_range = AddrRange(0, size=half)
    chip1_range = AddrRange(half, size=half)
    mem_ranges = [chip0_range, chip1_range]

system = System(mem_ranges=mem_ranges)
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
cpus = []
for i in range(num_cores):
    if args.cpu_type == "O3":
        cpu = ArmO3CPU(cpu_id=i)
    else:
        cpu = ArmTimingSimCPU(cpu_id=i)
    cpu.createThreads()
    cpu.workload = Process(cmd=[args.bin] + list(args.args or []))
    cpus.append(cpu)
system.cpu = cpus

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

else:  # R2
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
    )
    snf_mem_ranges = [chip0_range, chip1_range]

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
m5.instantiate()

print(
    f"Starting {topology} SE simulation, "
    f"{num_cores} cores, cpu-type={args.cpu_type}"
)
exit_event = m5.simulate(args.max_ticks)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
