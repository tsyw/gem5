# Copyright (c) 2026 The gem5 Contributors
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
Random MemTest stress for CHI C2C R2/R3 topologies.

This keeps the ruby_mem_test traffic model but swaps in the CHI C2C
hierarchy builders used by the R2/R3 experiment configs so topology
construction and replay pressure can be tested without a benchmark binary.
"""

import argparse
import math
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath


def _resolve_configs_dir():
    candidates = [os.path.join(m5.util.repoPath(), "configs")]

    src_root = os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", "")
    if src_root:
        candidates.append(os.path.join(src_root, "configs"))

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


def _split_addr_range(range_obj, num_parts):
    total = int(range_obj.size())
    part_size = total // num_parts
    start = int(range_obj.start)
    parts = []
    for index in range(num_parts):
        part_start = start + index * part_size
        if index < num_parts - 1:
            size = part_size
        else:
            size = total - index * part_size
        parts.append(AddrRange(start=part_start, size=size))
    return parts


_configs = _resolve_configs_dir()
addToPath(_configs)
addToPath(os.path.join(_configs, "ruby"))

from common import (  # noqa: E402
    MemConfig,
    ObjectList,
    Options,
)
from ruby import Ruby  # noqa: E402
from ruby.CHI_config import NoC_Params  # noqa: E402

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

from chi_c2c_four_chip import build_four_chip_system  # noqa: E402
from chi_c2c_two_chip import build_two_chip_system  # noqa: E402
from CHI_Mesh_XY import CHI_Mesh_XY  # noqa: E402

parser = argparse.ArgumentParser(
    description="ruby_mem_test-style stress for CHI C2C R2/R3",
    conflict_handler="resolve",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)
Options.addNoISAOptions(parser)
Ruby.define_options(parser)
parser.set_defaults(mem_type="SimpleMemory", mem_size="4GiB")

parser.add_argument(
    "--topology",
    default="R2",
    choices=["R2", "R3"],
    help="Chiplet topology under test",
)
parser.add_argument(
    "--maxloads", metavar="N", default=0, help="Stop after N loads"
)
parser.add_argument(
    "--progress",
    type=int,
    default=1000,
    metavar="NLOADS",
    help="Progress message interval",
)
parser.add_argument(
    "--functional",
    type=int,
    default=0,
    help="percentage of accesses that should be functional",
)
parser.add_argument(
    "--atomic",
    type=int,
    default=0,
    help="percentage of accesses that should be atomic",
)
parser.add_argument(
    "--suppress-func-errors",
    action="store_true",
    help="suppress panic when functional accesses fail",
)
parser.add_argument(
    "--mesh-rows",
    type=int,
    default=8,
    metavar="R",
    help="Rows in the CHI_Mesh_XY topology",
)
parser.add_argument(
    "--num-c2cgs",
    type=int,
    default=2,
    metavar="K",
    help="C2CGs per chip: R2 uses 2/4, R3 uses 3/6",
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
    help="C2C container latency in cycles",
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
    help="Number of C2CG transaction TBEs per gateway",
)
parser.add_argument(
    "--data-width",
    type=int,
    default=0,
    metavar="BYTES",
    help="Override CHI data channel width in bytes (0 = default).",
)

args = parser.parse_args()
if args.data_width > 0:
    NoC_Params.data_width = args.data_width
topology = args.topology.upper()

args.l1d_size = "256B"
args.l1i_size = "256B"
args.l2_size = "512B"
args.l3_size = "1KiB"
args.l1d_assoc = 2
args.l1i_assoc = 2
args.l2_assoc = 2
args.l3_assoc = 2

if args.link_bw_gbps > 0.0:
    args.container_latency = max(1, math.ceil(256.0 / args.link_bw_gbps))
    print(
        f"link-bw-gbps={args.link_bw_gbps:.1f} -> "
        f"container_latency={args.container_latency} cycles"
    )

block_size = 64
if args.num_cpus > block_size:
    print(
        "Error: Number of testers %d limited to %d because of false "
        "sharing" % (args.num_cpus, block_size)
    )
    sys.exit(1)

if topology == "R2" and args.num_cpus % 2 != 0:
    m5.fatal("R2 requires --num-cpus to be divisible by 2")
if topology == "R3" and args.num_cpus % 4 != 0:
    m5.fatal("R3 requires --num-cpus to be divisible by 4")

full_range = AddrRange(args.mem_size)
if topology == "R2":
    half = full_range.size() // 2
    chip0_range = AddrRange(start=0, size=half)
    chip1_range = AddrRange(start=half, size=half)
    mem_ranges = [chip0_range, chip1_range]
else:
    chip_ranges = _split_addr_range(full_range, 4)
    mem_ranges = chip_ranges

testers = [
    MemTest(
        max_loads=args.maxloads,
        percent_functional=args.functional,
        percent_uncacheable=0,
        percent_atomic=args.atomic,
        progress_interval=args.progress,
        suppress_func_errors=args.suppress_func_errors,
    )
    for _ in range(args.num_cpus)
]

cpu_parents = [SubSystem() for _ in range(args.num_cpus)]

system = System(
    cpu=cpu_parents,
    memtesters=testers,
    mem_ranges=mem_ranges,
)
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock,
    voltage_domain=system.voltage_domain,
)
system.mem_mode = "timing"
system.cache_line_size = args.cacheline_size

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

if topology == "R2":
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
        cpu_parents,
        chip0_range=chip0_range,
        chip1_range=chip1_range,
        container_latency=args.container_latency,
        num_c2cgs=args.num_c2cgs,
        txq_size=args.txq_size,
        num_tbes=args.num_tbes,
    )
    snf_mem_ranges = [chip0_range, chip1_range]
else:
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
        cpu_parents,
        chip_ranges=chip_ranges,
        container_latency=args.container_latency,
        num_c2cgs=args.num_c2cgs,
        txq_size=args.txq_size,
        num_tbes=args.num_tbes,
    )
    snf_mem_ranges = chip_ranges

mesh_topology = CHI_Mesh_XY(net_cntrls)
mesh_topology.makeTopology(
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

system.ruby.block_size_bytes = args.cacheline_size
system.ruby.memory_size_bits = 48
system.ruby.number_of_virtual_networks = (
    system.ruby.network.number_of_virtual_networks
)
system.ruby._cpu_ports = cpu_seqs
system.ruby.num_of_sequencers = len(cpu_seqs)
system.ruby.randomization = True

assert len(testers) == len(cpu_seqs)
for tester, seq in zip(testers, cpu_seqs):
    tester.port = seq.in_ports
    seq.deadlock_threshold = 5000000

for index, (snf_cntrl, mem_range) in enumerate(
    zip(snf_cntrls, snf_mem_ranges)
):
    mem_type = ObjectList.mem_list.get(args.mem_type)
    mem_intf = MemConfig.create_mem_intf(
        mem_type,
        mem_range,
        0,
        0,
        args.cacheline_size,
        0,
    )
    if issubclass(mem_type, DRAMInterface):
        mem_ctrl = MemCtrl(dram=mem_intf)
    else:
        mem_ctrl = mem_intf
    mem_ctrl.port = snf_cntrl.memory_out_port
    snf_cntrl.addr_ranges = [mem_intf.range]
    setattr(system, f"mem_ctrl{index}", mem_ctrl)

system.sys_port_proxy = RubyPortProxy(ruby_system=system.ruby)
system.system_port = system.sys_port_proxy.in_ports

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"

m5.ticks.setGlobalFrequency("1ns")
m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)

print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
