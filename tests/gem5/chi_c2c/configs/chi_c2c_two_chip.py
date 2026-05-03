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
Two-chip CHI C2C test configuration.

Creates two CHI sub-systems (Chip 0 and Chip 1), each with:
  - N RNFs (request nodes with L1/L2 caches, default 1)
  - 1 HNF (home node with L3 cache)
  - 1 SNF (memory controller)
  - 1 MN  (misc node for DVM)
  - K C2CGs (chip-to-chip gateways, default 1, configurable via num_c2cgs)

The C2CGs are wired together via wireC2CLink(). With K C2CGs per chip,
each C2CG pair (c2cg_i on chip0 ↔ c2cg_i on chip1) handles a
cache-line-address-hashed partition of the remote address range.

Address space split:
  Chip 0 HNF owns [0x00000000, 0x80000000)   (lower 2 GiB)
  Chip 1 HNF owns [0x80000000, 0x100000000)  (upper 2 GiB)

With K C2CGs per chip, remote traffic is hashed using
    c2cg_index = (addr >> 6) % K
and implemented with interleaved AddrRanges over the remote chip range.

Each RNF routes requests to the local HNF or the C2CG whose addr_range
covers the destination address.
"""

import os

import m5
from m5.objects import *
from m5.util import addToPath

_configs = os.path.join(m5.util.repoPath(), "configs")
if not os.path.isdir(os.path.join(_configs, "ruby")):
    _src = os.environ.get("THIRD_PARTY_GEM5_SRCS_HOME", "")
    if _src:
        _configs = os.path.join(_src, "configs")
addToPath(_configs)
# CHI_C2C_config uses bare "from CHI_config import ..." so ruby/ must
# also be on the path.
addToPath(os.path.join(_configs, "ruby"))
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


def _partition_range(range_obj, num_parts, block_size_bits):
    """Hash an AddrRange into power-of-two interleaved C2CG partitions."""
    if num_parts <= 0:
        raise ValueError("num_parts must be positive")
    if num_parts == 1:
        return [AddrRange(start=range_obj.start, size=range_obj.size())]
    if num_parts & (num_parts - 1):
        raise ValueError(
            "C2CG address hashing requires a power-of-two gateway count"
        )

    intlv_bits = num_parts.bit_length() - 1
    intlv_high_bit = block_size_bits + intlv_bits - 1
    return [
        AddrRange(
            start=range_obj.start,
            size=range_obj.size(),
            intlvHighBit=intlv_high_bit,
            intlvBits=intlv_bits,
            intlvMatch=i,
        )
        for i in range(num_parts)
    ]


def _build_chip(
    cpus,
    ruby_system,
    l1i_type,
    l1d_type,
    l2_type,
    cache_line_size,
    hnf_cache_cls,
    addr_range,
    remote_range,
    full_range,
    interleave_idx,
    num_c2cgs=1,
    num_tbes=32,
):
    """Build one chip's worth of CHI nodes.

    Args:
        cpus: list of SimObjects to serve as CPU parents (1 RNF each)
        num_c2cgs: number of C2CGs for this chip (default 1)
    """
    rnfs = []
    all_rnf_cntrls = []
    sequencers = []
    for cpu in cpus:
        rnf = CHI_RNF(
            [cpu],
            ruby_system,
            l1i_type,
            l1d_type,
            cache_line_size,
        )
        rnf.addPrivL2Cache(l2_type)
        rnfs.append(rnf)
        all_rnf_cntrls.extend(rnf.getAllControllers())
        sequencers.extend(rnf.getSequencers())

    CHI_HNF.createAddrRanges([addr_range], cache_line_size, [interleave_idx])
    hnf = CHI_HNF(interleave_idx, ruby_system, hnf_cache_cls, None)

    snf = CHI_SNF_MainMem(ruby_system, None, None)

    # Create num_c2cgs C2CGs, each handling a hash-selected remote partition.
    remote_partitions = _partition_range(
        remote_range,
        num_c2cgs,
        cache_line_size.bit_length() - 1,
    )
    c2cgs = [CHI_C2CG(ruby_system, [part]) for part in remote_partitions]
    all_c2cg_cntrls = []
    upstream_cache_destinations = [ctrl.version for ctrl in all_rnf_cntrls]
    for c2cg in c2cgs:
        c2cg.getAllControllers()[0].number_of_TBEs = num_tbes
        c2cg.getAllControllers()[
            0
        ].upstream_cache_destinations = upstream_cache_destinations
        all_c2cg_cntrls.extend(c2cg.getAllControllers())

    # Wire all C2CGs as extra upstream destinations for MN so that DVM
    # snoops (SnpDvmOp) are forwarded to all C2CGs for cross-chip propagation.
    mn = CHI_MN(
        ruby_system,
        all_rnf_cntrls,
        extra_upstream=all_c2cg_cntrls,
    )

    network_nodes = list(rnfs) + [hnf, snf, mn] + c2cgs
    all_cntrls = []
    network_cntrls = []
    for node in network_nodes:
        all_cntrls.extend(node.getAllControllers())
        network_cntrls.extend(node.getNetworkSideControllers())

    return (
        rnfs,
        hnf,
        snf,
        mn,
        c2cgs,
        network_nodes,
        all_cntrls,
        network_cntrls,
        sequencers,
        snf.getAllControllers(),
    )


def build_two_chip_system(
    ruby_system,
    system,
    options,
    cpus,
    chip0_range=None,
    chip1_range=None,
    container_latency=1,
    num_c2cgs=1,
    txq_size=0,
    num_tbes=32,
):
    """
    Build a two-chip CHI system with C2C gateways.

    Args:
        ruby_system: The RubySystem object
        system: The System object
        options: Parsed command-line options
        cpus: List of SimObjects to serve as CPU parents.
              Split evenly between chips (first half -> chip 0,
              second half -> chip 1). Must have even length.
        chip0_range: Address range for chip 0 (default: lower 2 GiB)
        chip1_range: Address range for chip 1 (default: upper 2 GiB)
        num_c2cgs: Number of C2CGs per chip (default 1). Each C2CG pair
                   (c2cg_i on chip0 ↔ c2cg_i on chip1) handles a
                   partition of the remote address range.
        txq_size: Max buffered granules in bridge TX queues (0 = unlimited).

    Returns:
        (network_nodes, network_cntrls, all_cntrls,
         cpu_sequencers, mem_cntrls, c2cg_pair)
        where c2cg_pair is (list_of_chip0_c2cgs, list_of_chip1_c2cgs)
    """
    if CHI_C2CG is None:
        m5.fatal("CHI C2C config classes not available")

    if chip0_range is None:
        chip0_range = AddrRange(start=0, size="2GiB")
    if chip1_range is None:
        chip1_range = AddrRange(start="2GiB", size="2GiB")

    full_range = [chip0_range, chip1_range]
    cache_line_size = system.cache_line_size.value
    params = NoC_Params

    class HNFCache(RubyCache):
        dataAccessLatency = 10
        tagAccessLatency = 2
        size = "256KiB"
        assoc = 8

    l1i_type = L1ICache(size=options.l1i_size, assoc=options.l1i_assoc)
    l1d_type = L1DCache(size=options.l1d_size, assoc=options.l1d_assoc)
    l2_type = L2Cache(size=options.l2_size, assoc=options.l2_assoc)

    cpus_per_chip = len(cpus) // 2
    chip_args = [
        (cpus[:cpus_per_chip], chip0_range, chip1_range, 0),
        (cpus[cpus_per_chip:], chip1_range, chip0_range, 1),
    ]

    network_nodes = []
    network_cntrls = []
    all_cntrls = []
    cpu_sequencers = []
    mem_cntrls = []
    rnfs = []
    hnfs = []
    snfs = []
    mns = []
    # c2cgs[chip_i] = list of K C2CGs for chip i
    c2cgs = []

    for chip_cpus, addr_range, remote_range, idx in chip_args:
        (
            chip_rnfs,
            hnf,
            snf,
            mn,
            chip_c2cgs,
            nodes,
            cntrls,
            net_cntrls,
            seqs,
            snf_cntrls,
        ) = _build_chip(
            chip_cpus,
            ruby_system,
            l1i_type,
            l1d_type,
            l2_type,
            cache_line_size,
            HNFCache,
            addr_range,
            remote_range,
            full_range,
            idx,
            num_c2cgs=num_c2cgs,
            num_tbes=num_tbes,
        )
        network_nodes.extend(nodes)
        network_cntrls.extend(net_cntrls)
        all_cntrls.extend(cntrls)
        cpu_sequencers.extend(seqs)
        mem_cntrls.extend(snf_cntrls)
        rnfs.append(chip_rnfs)
        hnfs.append(hnf)
        snfs.append(snf)
        mns.append(mn)
        c2cgs.append(chip_c2cgs)

    # Flatten rnfs for parenting (ruby_system.rnf expects flat list)
    all_rnfs = [rnf for chip_rnfs in rnfs for rnf in chip_rnfs]
    ruby_system.rnf = all_rnfs
    ruby_system.hnf = hnfs
    ruby_system.snf = snfs
    ruby_system.mn = mns
    # Flatten c2cgs for parenting
    ruby_system.c2cg = [c for chip in c2cgs for c in chip]

    # Wire K C2C links: c2cgs[0][i] ↔ c2cgs[1][i]
    all_bridges = []
    for i in range(num_c2cgs):
        bridges = wireC2CLink(
            c2cgs[0][i],
            c2cgs[1][i],
            ruby_system,
            container_latency=container_latency,
            txq_size=txq_size,
        )
        if bridges is not None:
            setattr(system, f"c2c_bridge_a2b_{i}", bridges[0])
            setattr(system, f"c2c_bridge_b2a_{i}", bridges[1])
            all_bridges.append(bridges)

    # Downstream routing
    for i in range(2):
        all_c2cg_cntrls = []
        for c2cg in c2cgs[i]:
            all_c2cg_cntrls.extend(c2cg.getAllControllers())
        downstream = hnfs[i].getAllControllers() + all_c2cg_cntrls
        for rnf in rnfs[i]:
            rnf.setDownstream(downstream)
        hnfs[i].setDownstream(snfs[i].getAllControllers())
        # Each C2CG forwards inbound remote requests to the local HNF
        for c2cg in c2cgs[i]:
            c2cg.setDownstream(hnfs[i].getAllControllers())

    # Data message size
    for cntrl in all_cntrls:
        cntrl.data_channel_size = params.data_width

    # Network config
    ruby_system.network.number_of_virtual_networks = 4
    ruby_system.network.control_msg_size = params.cntrl_msg_size
    ruby_system.network.data_msg_size = params.data_width
    if hasattr(options, "network") and options.network == "simple":
        ruby_system.network.buffer_size = params.router_buffer_size

    return (
        network_nodes,
        network_cntrls,
        all_cntrls,
        cpu_sequencers,
        mem_cntrls,
        (c2cgs[0], c2cgs[1]),
    )
