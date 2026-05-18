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
Driver config for CHI C2C integration tests.

Builds a two-chip CHI system with C2C gateways and runs directed
traffic via RubyDirectedTester.  Each chip has N RNFs (default 1),
1 HNF, 1 SNF, 1 MN, and 1 C2CG.  The test-type flag selects the
traffic pattern.
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# repoPath() works when running from a source-tree build;
# fall back to THIRD_PARTY_GEM5_SRCS_HOME for EXTRAS builds.
_configs = os.path.join(m5.util.repoPath(), "configs")
if not os.path.isdir(os.path.join(_configs, "common")):
    _src = os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", "")
    if _src:
        _configs = os.path.join(_src, "configs")
addToPath(_configs)
from common import (
    MemConfig,
    ObjectList,
    Options,
)
from ruby import Ruby
from ruby.CHI_config import NoC_Params

try:
    _here = os.path.dirname(os.path.abspath(__file__))
except NameError:
    # When loaded via exec/importlib without __file__ being set
    _here = os.path.join(
        os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", m5.util.repoPath()),
        "tests",
        "gem5",
        "chi_c2c",
        "configs",
    )
sys.path.insert(0, _here)
from chi_c2c_two_chip import build_two_chip_system

parser = argparse.ArgumentParser(description="CHI C2C integration test driver")
Options.addNoISAOptions(parser)
Ruby.define_options(parser)

parser.add_argument(
    "--test-type",
    default="SeriesGetx",
    choices=[
        "SeriesGetx",
        "SeriesGets",
        "SeriesGetMixed",
        "Invalidate",
    ],
)
parser.add_argument(
    "--requests",
    type=int,
    default=100,
    metavar="N",
)
parser.add_argument(
    "--percent-writes",
    type=int,
    default=50,
)
parser.add_argument(
    "--cpus-per-chip",
    type=int,
    default=1,
    metavar="N",
    help="Number of RNFs (CPUs) per chip (default 1)",
)
parser.add_argument(
    "--container-latency",
    type=int,
    default=1,
    metavar="CYCLES",
    help="C2C packetizer bridge container latency (default 1)",
)
parser.add_argument(
    "--num-c2cgs",
    type=int,
    default=1,
    metavar="K",
    help="Number of C2C gateways per chip (default 1)",
)
parser.add_argument(
    "--link-bw-gbps",
    type=float,
    default=0.0,
    metavar="BW",
    help="C2C link bandwidth in GB/s (0 = use --container-latency directly). "
    "When non-zero, container_latency is computed as "
    "ceil(256 / bw_gbps) cycles (assumes 1GHz clock).",
)
parser.add_argument(
    "--txq-size",
    type=int,
    default=0,
    metavar="N",
    help="Max buffered granules in C2C bridge TX queues (0 = unlimited).",
)
parser.add_argument(
    "--issue-window",
    type=int,
    default=1,
    metavar="N",
    help="Max outstanding SeriesRequestGenerator requests (default 1).",
)
parser.add_argument(
    "--disable-read-merge",
    action="store_true",
    help="Keep C2CG same-address ReadShared merge/fanout disabled.",
)
parser.add_argument(
    "--enable-read-merge",
    action="store_true",
    help="Enable C2CG same-address ReadShared merge/fanout.",
)
parser.add_argument(
    "--cross-snf",
    action="store_true",
    help="Place each SNF behind the peer C2CG to test HNF-to-SNF flows.",
)
parser.add_argument(
    "--enable-dmt-early-dealloc",
    action="store_true",
    help="Enable HNF ReadNoSnpSep/ReadReceipt DMT early deallocation.",
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

import math

# Compute container_latency from bandwidth if specified
# Container = 256 bytes; at 1GHz clock, latency_cycles = ceil(256 / bw_gbps)
if args.link_bw_gbps > 0.0:
    args.container_latency = max(1, math.ceil(256.0 / args.link_bw_gbps))

# Total CPUs = 2 chips * cpus_per_chip
args.num_cpus = 2 * args.cpus_per_chip
generator_num_cpus = args.cpus_per_chip if args.cross_snf else args.num_cpus
# Force 2 dirs so setup_memory_controllers handles 2 SNFs
args.num_dirs = 2
# Use Crossbar topology for flat two-chip network
args.topology = "Crossbar"

if args.test_type == "SeriesGetx":
    generator = SeriesRequestGenerator(
        num_cpus=generator_num_cpus,
        percent_writes=100,
        issue_window=args.issue_window,
    )
elif args.test_type == "SeriesGets":
    generator = SeriesRequestGenerator(
        num_cpus=generator_num_cpus,
        percent_writes=0,
        issue_window=args.issue_window,
    )
elif args.test_type == "SeriesGetMixed":
    generator = SeriesRequestGenerator(
        num_cpus=generator_num_cpus,
        percent_writes=args.percent_writes,
        issue_window=args.issue_window,
    )
elif args.test_type == "Invalidate":
    generator = InvalidateGenerator(num_cpus=generator_num_cpus)
else:
    m5.fatal("Unknown test type: %s" % args.test_type)

chip0_range = AddrRange(start=0, size="2GiB")
chip1_range = AddrRange(start="2GiB", size="2GiB")

system = System(mem_ranges=[chip0_range, chip1_range])
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock,
    voltage_domain=system.voltage_domain,
)

system.tester = RubyDirectedTester(
    requests_to_complete=args.requests, generator=generator
)

# CHI_RNF attaches sequencers/caches as children of these
system.cpu = [SubSystem() for _ in range(args.num_cpus)]

system.ruby = RubySystem()
system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock,
    voltage_domain=system.voltage_domain,
)

system.ruby.network = SimpleNetwork(
    ruby_system=system.ruby,
    topology=args.topology,
    routers=[],
    ext_links=[],
    int_links=[],
    netifs=[],
)

(
    network_nodes,
    network_cntrls,
    all_cntrls,
    cpu_sequencers,
    mem_cntrls,
    c2cg_pair,
) = build_two_chip_system(
    system.ruby,
    system,
    args,
    system.cpu,
    chip0_range,
    chip1_range,
    container_latency=args.container_latency,
    num_c2cgs=args.num_c2cgs,
    txq_size=args.txq_size,
    cross_snf=args.cross_snf,
)

for hnf in system.ruby.hnf:
    for controller in hnf.getAllControllers():
        controller.enable_DMT_early_dealloc = args.enable_dmt_early_dealloc

for c2cg_list in c2cg_pair:
    for c2cg in c2cg_list:
        for controller in c2cg.getAllControllers():
            controller.readmerge_enabled = (
                args.enable_read_merge and not args.disable_read_merge
            )

# Build topology — Crossbar expects flat controller list
topology = Ruby.create_topology(network_cntrls, args)
topology.makeTopology(
    args,
    system.ruby.network,
    SimpleIntLink,
    SimpleExtLink,
    Switch,
)
system.ruby.network.setup_buffers()

# Set up DRAM backing for each SNF controller.
# Unlike Ruby.setup_memory_controllers, we do NOT interleave across dirs.
# In cross-SNF mode each chip's SNF backs the peer chip's home range.
system.ruby.block_size_bytes = args.cacheline_size
system.ruby.memory_size_bits = 48

_mem_ctrls = []
if args.cross_snf:
    mem_ranges = [chip1_range, chip0_range]
else:
    mem_ranges = [chip0_range, chip1_range]
for i, (snf_cntrl, mem_range) in enumerate(zip(mem_cntrls, mem_ranges)):
    mem_type = ObjectList.mem_list.get(args.mem_type)
    dram_intf = MemConfig.create_mem_intf(
        mem_type,
        mem_range,
        0,
        0,
        args.cacheline_size,
        0,
    )
    if issubclass(mem_type, DRAMInterface):
        mem_ctrl = MemCtrl(dram=dram_intf)
    else:
        mem_ctrl = dram_intf
    mem_ctrl.port = snf_cntrl.memory_out_port
    snf_cntrl.addr_ranges = [dram_intf.range]
    _mem_ctrls.append(mem_ctrl)

# Parent mem controllers under system
for i, mc in enumerate(_mem_ctrls):
    setattr(system, f"mem_ctrl{i}", mc)

# Port proxy for system port (SE mode)
sys_port_proxy = RubyPortProxy(ruby_system=system.ruby)
system.sys_port_proxy = sys_port_proxy
system.system_port = system.sys_port_proxy.in_ports

# Wire tester ports to CPU sequencers
system.ruby.number_of_virtual_networks = (
    system.ruby.network.number_of_virtual_networks
)
system.ruby._cpu_ports = cpu_sequencers
system.ruby.num_of_sequencers = len(cpu_sequencers)
assert len(cpu_sequencers) == args.num_cpus

for seq in cpu_sequencers:
    system.tester.cpuPort = seq.in_ports

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"
m5.ticks.setGlobalFrequency("1ns")
m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)
print(
    "Exiting @ tick",
    m5.curTick(),
    "because",
    exit_event.getCause(),
)
