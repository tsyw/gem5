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
Full-system CHI C2C configuration matching Schätzle et al. Table I.

Three topology variants (select with --topology):
  R1  Monolithic  64 cores, 1 chip
  R2  2 chiplets  32+32 cores, 2 chips,  {2,4} C2CGs/chip
  R3  4 chiplets  16×4 cores, 4 chips,   {3,6} C2CGs/chip  [stub, not runnable]

Hardware parameters (Table I):
  CPU:  ArmO3CPU, SVE 2×256-bit (sve_vl=4), 3 GHz
  L1I:  64 KiB, 4-way
  L1D:  64 KiB, 4-way
  L2:   512 KiB, 8-way  (private per core)
  SLC:  1 MiB, 16-way   (shared last-level, one per chip / HNF)
  Mem:  HBM2 (HBM_2000_4H_1x64), two pseudo-channels per SNF

Usage (requires ARM64 disk image + vmlinux kernel):
  gem5.debug chi_c2c_fs.py \\
      --topology R2 --num-c2cgs 2 --num-cores 64 \\
      --link-bw-gbps 256 --txq-size 256 \\
      --kernel vmlinux.arm64 --disk-image aarch64.img \\
      --script run_stream.sh

Required resources (not included):
  - vmlinux.arm64  : ARM64 Linux kernel with NUMA + SVE support
  - aarch64.img    : Disk image with numactl, OpenMP runtime, benchmarks
  See https://resources.gem5.org or build from gem5-resources repo.

Limitations:
  - R3 (4-chiplet) requires a 4-chip topology extension (not yet implemented).
  - Full application workloads require disk images and are a separate milestone
    per binding decision D15 (see docs/final/CHI_C2C_Final_Design_and_Verification.md).
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
addToPath(os.path.join(_configs, "example", "arm"))
addToPath(os.path.join(_configs, "ruby"))

import devices
from common import (
    ObjectList,
    Options,
    SysPaths,
)
from common.cores.arm import O3_ARM_v7a
from ruby import Ruby
from ruby.CHI_config import (
    CHI_HNF,
    CHI_MN,
    CHI_RNF,
    CHI_SNF_MainMem,
    L1DCache,
    L1ICache,
    L2Cache,
    NoC_Params,
    Versions,
)

try:
    from ruby.CHI_C2C_config import (
        CHI_C2CG,
        wireC2CLink,
    )
except ImportError:
    CHI_C2CG = None
    wireC2CLink = None

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
from chi_c2c_two_chip import (
    _partition_range,
    build_two_chip_system,
)

# ------------------------------------------------------------------
# Paper Table I hardware parameters
# ------------------------------------------------------------------
_CPU_FREQ = "3GHz"
_L1I_SIZE = "64KiB"
_L1I_ASSOC = 4
_L1D_SIZE = "64KiB"
_L1D_ASSOC = 4
_L2_SIZE = "512KiB"
_L2_ASSOC = 8
_SLC_SIZE = "1MiB"
_SLC_ASSOC = 16
# SVE vector length: 2×256-bit = 4 quadwords of 128 bits
_SVE_VL = 4


# ------------------------------------------------------------------
# Cache class overrides
# ------------------------------------------------------------------
class _PaperL1I(L1ICache):
    size = _L1I_SIZE
    assoc = _L1I_ASSOC


class _PaperL1D(L1DCache):
    size = _L1D_SIZE
    assoc = _L1D_ASSOC


class _PaperL2(L2Cache):
    size = _L2_SIZE
    assoc = _L2_ASSOC


class _PaperSLC(RubyCache):
    """Shared last-level cache backing the HNF."""

    size = _SLC_SIZE
    assoc = _SLC_ASSOC
    dataAccessLatency = 10
    tagAccessLatency = 2


# ------------------------------------------------------------------
# CPU: Armv8 O3 with SVE
# ------------------------------------------------------------------
class _PaperO3CPU(O3_ARM_v7a.O3_ARM_v7a_3):
    """O3 ARM CPU with SVE 2×256-bit (Table I)."""

    sve_vl = _SVE_VL


# ------------------------------------------------------------------
# HBM2 memory controller creation
# HBMCtrl manages two HBM pseudo-channels.  Each chip gets one HBMCtrl
# (two HBM_2000_4H_1x64 interfaces).  The controller's port is bound
# to the SNF's memory_out_port after topology construction.
# ------------------------------------------------------------------
def _make_hbm2_ctrl(addr_range):
    """Return an HBMCtrl for one chip, covering addr_range."""
    ctrl = HBMCtrl()
    ctrl.dram = HBM_2000_4H_1x64(range=addr_range)
    ctrl.dram_2 = HBM_2000_4H_1x64(range=addr_range)
    return ctrl


# ------------------------------------------------------------------
# R1: Monolithic single-chip CHI system (EX-008: NUMA multi-HNF/SNF)
# ------------------------------------------------------------------
def _build_r1(ruby_system, cache_line_size, cpus, mem_range, num_hnfs=4):
    """
    Build R1 (monolithic) CHI hierarchy with NUMA-aware HNF/SNF layout.

    num_hnfs controls the number of NUMA domains (paper Table I uses 4).
    Each HNF covers an interleaved portion of the full address space.
    Each SNF backs a contiguous NUMA-sized sub-range with its own HBM2 ctrl.

    Returns (network_nodes, network_cntrls, all_cntrls, cpu_seqs, snf_cntrls)
    where snf_cntrls is a list of CHI_SNF controller objects whose
    memory_out_port and addr_ranges must be set by the caller.
    """
    # Interleaved HNF ranges (cache-line granularity)
    hnf_indices = list(range(num_hnfs))
    CHI_HNF.createAddrRanges([mem_range], cache_line_size, hnf_indices)
    hnfs = [CHI_HNF(i, ruby_system, _PaperSLC, None) for i in hnf_indices]

    # Contiguous SNF ranges — one per NUMA domain (EX-008)
    numa_size = mem_range.size() // num_hnfs
    snfs = []
    numa_ranges = []
    for i in range(num_hnfs):
        snf = CHI_SNF_MainMem(ruby_system, None, None)
        snfs.append(snf)
        numa_ranges.append(
            AddrRange(mem_range.start + i * numa_size, size=numa_size)
        )

    mn = CHI_MN(ruby_system)

    all_hnf_cntrls = []
    for hnf in hnfs:
        all_hnf_cntrls.extend(hnf.getAllControllers())

    all_snf_cntrls = []
    for snf in snfs:
        all_snf_cntrls.extend(snf.getAllControllers())

    rnfs = []
    for cpu in cpus:
        rnf = CHI_RNF(
            [cpu],
            ruby_system,
            _PaperL1I,
            _PaperL1D,
            _PaperL2,
            cache_line_size,
        )
        rnf.setDownstream(all_hnf_cntrls)
        rnfs.append(rnf)

    for hnf in hnfs:
        hnf.setDownstream(all_snf_cntrls)

    network_nodes = list(rnfs) + hnfs + snfs + [mn]
    all_cntrls = []
    network_cntrls = []
    for node in network_nodes:
        all_cntrls.extend(node.getAllControllers())
        network_cntrls.extend(node.getNetworkSideControllers())

    cpu_seqs = []
    for rnf in rnfs:
        cpu_seqs.extend(rnf.getSequencers())

    ruby_system.rnf = rnfs
    ruby_system.hnf = hnfs
    ruby_system.snf = snfs
    ruby_system.mn = [mn]

    return (
        network_nodes,
        network_cntrls,
        all_cntrls,
        cpu_seqs,
        all_snf_cntrls,
        numa_ranges,
    )


# ------------------------------------------------------------------
# System construction
# ------------------------------------------------------------------
def build_fs_system(args):
    topology = args.topology.upper()
    num_cores = args.num_cores

    if topology == "R3":
        raise NotImplementedError(
            "R3 (4-chiplet) requires a 4-chip topology extension.\n"
            "Use R1 or R2 for now."
        )

    # ------ ARM platform system ------
    # ArmRubySystem sets up: realview platform, iobus, _dma_ports, _mem_ports
    system = devices.ArmRubySystem(
        args.mem_size,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
    )
    if args.script:
        system.workload.readfile = args.script

    system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
    system.clk_domain = SrcClockDomain(
        clock=_CPU_FREQ, voltage_domain=system.voltage_domain
    )
    system.mem_mode = "timing"
    system.cache_line_size = 64

    # ------ CPUs ------
    cpus = []
    for i in range(num_cores):
        cpu = _PaperO3CPU(cpu_id=i)
        cpu.createThreads()
        cpu.clk_domain = SrcClockDomain(
            clock=_CPU_FREQ, voltage_domain=system.voltage_domain
        )
        cpus.append(cpu)
    system.cpu = cpus

    for cpu in cpus:
        cpu.createInterruptController()

    # ------ Disk image ------
    if args.disk_image:
        system.pci_devices = [
            PciVirtIO(
                vio=VirtIOBlock(
                    image=CowDiskImage(
                        child=RawDiskImage(
                            image_file=args.disk_image, read_only=True
                        )
                    )
                )
            )
        ]
        for dev in system.pci_devices:
            system.realview.attachPciDevice(dev, system.iobus)

    # ------ Ruby system ------
    system.ruby = RubySystem()
    system.ruby.clk_domain = SrcClockDomain(
        clock=args.ruby_clock, voltage_domain=system.voltage_domain
    )
    system.ruby.network = SimpleNetwork(
        ruby_system=system.ruby,
        topology=args.ruby_topology,
        routers=[],
        ext_links=[],
        int_links=[],
        netifs=[],
    )

    # ------ CHI topology ------
    if topology == "R1":
        mem_range = AddrRange(size=args.mem_size)
        (
            net_nodes,
            net_cntrls,
            all_cntrls,
            cpu_seqs,
            snf_cntrls,
            mem_ranges,
        ) = _build_r1(
            system.ruby,
            args.cacheline_size,
            cpus,
            mem_range,
            num_hnfs=args.num_hnfs,
        )

    else:  # R2
        # args.mem_size is a string like "8GiB"; split evenly via AddrRange
        full_range = AddrRange(size=args.mem_size)
        half = full_range.size() // 2
        chip0_range = AddrRange(0, size=half)
        chip1_range = AddrRange(half, size=half)
        mem_ranges = [chip0_range, chip1_range]

        (
            net_nodes,
            net_cntrls,
            all_cntrls,
            cpu_seqs,
            snf_cntrls,  # already a flat list of SNF controller objects
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

    # ------ Ruby network topology ------
    ruby_topology = Ruby.create_topology(net_cntrls, args)
    ruby_topology.makeTopology(
        args,
        system.ruby.network,
        SimpleIntLink,
        SimpleExtLink,
        Switch,
    )
    system.ruby.network.setup_buffers()

    # ------ Ruby system parameters ------
    system.ruby.block_size_bytes = args.cacheline_size
    system.ruby.memory_size_bits = 48
    system.ruby.number_of_virtual_networks = (
        system.ruby.network.number_of_virtual_networks
    )
    system.ruby._cpu_ports = cpu_seqs
    system.ruby.num_of_sequencers = len(cpu_seqs)

    # ------ Wire sequencers to CPUs ------
    assert (
        len(cpu_seqs) == num_cores
    ), f"Expected {num_cores} sequencers, got {len(cpu_seqs)}"
    for cpu, seq in zip(cpus, cpu_seqs):
        seq.connectCpuPorts(cpu)

    # ------ HBM2 memory controllers (one per chip / SNF) ------
    mem_ctrls = []
    for i, (snf_cntrl, mem_range) in enumerate(zip(snf_cntrls, mem_ranges)):
        ctrl = _make_hbm2_ctrl(mem_range)
        ctrl.port = snf_cntrl.memory_out_port
        snf_cntrl.addr_ranges = [mem_range]
        setattr(system, f"mem_ctrl{i}", ctrl)
        mem_ctrls.append(ctrl)

    # ------ Boot loader ------
    system.realview.setupBootLoader(system, SysPaths.binary)

    return system


# ------------------------------------------------------------------
# CLI argument parsing
# ------------------------------------------------------------------
# CLI argument parsing
# ------------------------------------------------------------------
def _add_fs_options(parser):
    parser.add_argument(
        "--topology",
        default="R2",
        choices=["R1", "R2", "R3"],
        help="Chiplet topology from Table I (default R2)",
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
            "HNF/SNF count for R1 NUMA domains (EX-008). "
            "Must be a power of 2. Paper Table I: R1=4, R2=2/chip, R3=1/chip."
        ),
    )
    parser.add_argument(
        "--num-c2cgs",
        type=int,
        default=2,
        help="C2CGs per chip: R2 uses {2,4}, R3 uses {3,6} (default 2)",
    )
    parser.add_argument(
        "--container-latency",
        type=int,
        default=1,
        metavar="CYCLES",
        help="C2C container latency in cycles (default 1 = unlimited BW)",
    )
    parser.add_argument(
        "--link-bw-gbps",
        type=float,
        default=0.0,
        metavar="BW",
        help=(
            "C2C unidirectional link BW in GB/s; overrides "
            "--container-latency. Paper sweeps 64-512 GB/s."
        ),
    )
    parser.add_argument(
        "--txq-size",
        type=int,
        default=0,
        metavar="GRANULES",
        help="C2C TX queue size in granules (0=unlimited; paper: 128/256)",
    )
    # Note: --mem-size (str, default 512MiB) added by Options.addNoISAOptions
    parser.add_argument(
        "--kernel",
        required=True,
        help="Path to ARM64 Linux kernel (vmlinux.arm64)",
    )
    parser.add_argument(
        "--disk-image",
        default=None,
        help="Path to ARM64 disk image; must include numactl + OpenMP",
    )
    parser.add_argument(
        "--script",
        default=None,
        help="Boot script to run inside the simulated system",
    )
    parser.add_argument(
        "--ruby-topology",
        default="Crossbar",
        help="Ruby network topology class (default Crossbar)",
    )
    # Cache size overrides (paper defaults if not specified)
    parser.add_argument("--l1i-size", default=_L1I_SIZE)
    parser.add_argument("--l1i-assoc", type=int, default=_L1I_ASSOC)
    parser.add_argument("--l1d-size", default=_L1D_SIZE)
    parser.add_argument("--l1d-assoc", type=int, default=_L1D_ASSOC)
    parser.add_argument("--l2-size", default=_L2_SIZE)
    parser.add_argument("--l2-assoc", type=int, default=_L2_ASSOC)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description=("CHI C2C full-system config — Schätzle et al. Table I")
    )
    Options.addNoISAOptions(parser)
    Ruby.define_options(parser)
    _add_fs_options(parser)

    args = parser.parse_args()

    # Convert bandwidth to latency if specified
    if args.link_bw_gbps > 0:
        # At 3 GHz: 1 cycle = 1/3 ns; 256B @ BW GB/s takes 256/BW ns
        latency_ns = 256.0 / args.link_bw_gbps
        args.container_latency = max(1, math.ceil(latency_ns * 3.0))
        print(
            f"link-bw-gbps={args.link_bw_gbps:.1f} → "
            f"container_latency={args.container_latency} cycles"
        )

    system = build_fs_system(args)

    root = Root(full_system=True, system=system)
    m5.instantiate()

    print(
        f"Starting {args.topology} simulation, "
        f"{args.num_cores} cores @ {_CPU_FREQ}"
    )
    exit_event = m5.simulate()
    print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
