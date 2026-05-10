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
Four-chip CHI C2C configuration.

Creates four CHI sub-systems (Chip 0..3) connected with a direct full-mesh
C2C topology. Each chip owns a contiguous local memory range and instantiates
direct C2CG links to every remote chip.

The paper's R3 configuration uses 3 or 6 C2CGs per chip:
  - 3 C2CGs/chip: one gateway per remote chip
  - 6 C2CGs/chip: two gateways per remote chip

This builder implements that model by requiring num_c2cgs to be a multiple of 3
and hashing each remote chip range across the gateways assigned to it.
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
addToPath(os.path.join(_configs, "ruby"))

from chi_c2c_two_chip import _partition_range
from ruby.CHI_config import (
    CHI_HNF,
    CHI_MN,
    CHI_RNF,
    CHI_SNF_MainMem,
    L1DCache,
    L1ICache,
    L2Cache,
    NoC_Params,
)

try:
    from ruby.CHI_C2C_config import (
        CHI_C2CG,
        wireC2CLink,
    )
except ImportError:
    CHI_C2CG = None
    wireC2CLink = None


def _build_chip(
    cpus,
    ruby_system,
    l1i_type,
    l1d_type,
    l2_type,
    cache_line_size,
    hnf_cache_cls,
    addr_range,
    remote_targets,
    interleave_idx,
    gateways_per_remote,
    num_tbes,
):
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

    c2cgs = {}
    all_c2cg_cntrls = []
    upstream_cache_destinations = [ctrl.version for ctrl in all_rnf_cntrls]
    for remote_chip, remote_range in remote_targets:
        chip_gateways = []
        for part in _partition_range(
            remote_range,
            gateways_per_remote,
            cache_line_size.bit_length() - 1,
        ):
            c2cg = CHI_C2CG(ruby_system, [part])
            c2cg.getAllControllers()[0].number_of_TBEs = num_tbes
            c2cg.getAllControllers()[
                0
            ].upstream_cache_destinations = upstream_cache_destinations
            chip_gateways.append(c2cg)
            all_c2cg_cntrls.extend(c2cg.getAllControllers())
        c2cgs[remote_chip] = chip_gateways

    mn = CHI_MN(
        ruby_system,
        all_rnf_cntrls,
        extra_upstream=all_c2cg_cntrls,
    )

    network_nodes = list(rnfs) + [hnf, snf, mn]
    for remote_chip in sorted(c2cgs):
        network_nodes.extend(c2cgs[remote_chip])

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


def build_four_chip_system(
    ruby_system,
    system,
    options,
    cpus,
    chip_ranges=None,
    container_latency=1,
    num_c2cgs=3,
    txq_size=0,
    num_tbes=32,
):
    if CHI_C2CG is None:
        m5.fatal("CHI C2C config classes not available")

    if len(cpus) % 4 != 0:
        m5.fatal("Four-chip system requires CPU count divisible by 4")

    if num_c2cgs <= 0 or num_c2cgs % 3 != 0:
        m5.fatal(
            "Four-chip system requires --num-c2cgs to be a multiple of 3 "
            "(paper values: 3 or 6)"
        )

    if chip_ranges is None:
        chip_ranges = [
            AddrRange(start=i * 0x40000000, size=0x40000000) for i in range(4)
        ]

    cache_line_size = system.cache_line_size.value
    params = NoC_Params
    gateways_per_remote = num_c2cgs // 3

    class HNFCache(RubyCache):
        dataAccessLatency = 10
        tagAccessLatency = 2
        size = "256KiB"
        assoc = 8

    l1i_type = L1ICache(size=options.l1i_size, assoc=options.l1i_assoc)
    l1d_type = L1DCache(size=options.l1d_size, assoc=options.l1d_assoc)
    l2_type = L2Cache(size=options.l2_size, assoc=options.l2_assoc)

    cpus_per_chip = len(cpus) // 4

    network_nodes = []
    network_cntrls = []
    all_cntrls = []
    cpu_sequencers = []
    mem_cntrls = []
    rnfs = []
    hnfs = []
    snfs = []
    mns = []
    c2cgs = []

    for chip_idx, addr_range in enumerate(chip_ranges):
        chip_cpus = cpus[
            chip_idx * cpus_per_chip : (chip_idx + 1) * cpus_per_chip
        ]
        remote_targets = [
            (remote_idx, chip_ranges[remote_idx])
            for remote_idx in range(len(chip_ranges))
            if remote_idx != chip_idx
        ]
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
            remote_targets,
            chip_idx,
            gateways_per_remote,
            num_tbes,
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

    ruby_system.rnf = [rnf for chip_rnfs in rnfs for rnf in chip_rnfs]
    ruby_system.hnf = hnfs
    ruby_system.snf = snfs
    ruby_system.mn = mns
    ruby_system.c2cg = [
        gateway
        for chip_gateways in c2cgs
        for remote_chip in sorted(chip_gateways)
        for gateway in chip_gateways[remote_chip]
    ]

    for src_chip in range(4):
        for dst_chip in range(src_chip + 1, 4):
            for gateway_idx in range(gateways_per_remote):
                forward = c2cgs[src_chip][dst_chip][gateway_idx]
                reverse = c2cgs[dst_chip][src_chip][gateway_idx]
                bridges = wireC2CLink(
                    forward,
                    reverse,
                    ruby_system,
                    container_latency=container_latency,
                    txq_size=txq_size,
                )
                if bridges is not None:
                    setattr(
                        system,
                        f"c2c_bridge_c{src_chip}to{dst_chip}_{gateway_idx}",
                        bridges[0],
                    )
                    setattr(
                        system,
                        f"c2c_bridge_c{dst_chip}to{src_chip}_{gateway_idx}",
                        bridges[1],
                    )

    peer_cache_destinations = []
    for chip_rnfs in rnfs:
        peer_cache_destinations.append(
            [
                ctrl.version
                for rnf in chip_rnfs
                for ctrl in rnf.getAllControllers()
            ]
        )

    for chip_idx in range(4):
        all_c2cg_cntrls = []
        for remote_chip in sorted(c2cgs[chip_idx]):
            for c2cg in c2cgs[chip_idx][remote_chip]:
                all_c2cg_cntrls.extend(c2cg.getAllControllers())
        downstream = hnfs[chip_idx].getAllControllers() + all_c2cg_cntrls
        for rnf in rnfs[chip_idx]:
            rnf.setDownstream(downstream)
        hnfs[chip_idx].setDownstream(snfs[chip_idx].getAllControllers())
        for remote_chip in sorted(c2cgs[chip_idx]):
            for c2cg in c2cgs[chip_idx][remote_chip]:
                c2cg.getAllControllers()[
                    0
                ].peer_upstream_cache_destinations = list(
                    peer_cache_destinations[remote_chip]
                )
                c2cg.setDownstream(hnfs[chip_idx].getAllControllers())

    for cntrl in all_cntrls:
        cntrl.data_channel_size = params.data_width

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
        c2cgs,
    )
