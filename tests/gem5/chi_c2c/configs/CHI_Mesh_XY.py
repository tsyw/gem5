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
CHI_Mesh_XY: flexible 2-D mesh topology for CHI C2C experiments.

Unlike the standard Mesh_XY, this topology:
  - Does NOT require len(nodes) % num_routers == 0
  - Sizes the mesh from len(self.nodes) directly, not options.num_cpus
  - Pads with empty routers (no ext_links) when nodes < rows*cols
  - Assigns controllers 1:1 to routers (one controller per router)
  - Horizontal links weight=1, vertical links weight=2 (XY routing)

Use add_options(parser) to register --mesh-rows before parsing.
"""

import math

from topologies.BaseTopology import SimpleTopology

from m5.objects import *
from m5.params import *


def add_options(parser):
    try:
        parser.add_argument(
            "--mesh-rows",
            type=int,
            default=8,
            metavar="R",
            help="Number of rows in the CHI_Mesh_XY topology (default 8)",
        )
    except Exception:
        pass


class CHI_Mesh_XY(SimpleTopology):
    description = "CHI_Mesh_XY"

    def __init__(self, controllers):
        self.nodes = controllers

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes
        num_nodes = len(nodes)

        rows = getattr(options, "mesh_rows", 8)
        if rows <= 0:
            rows = 1
        cols = math.ceil(num_nodes / rows)
        num_routers = rows * cols

        link_latency = options.link_latency
        router_latency = options.router_latency

        routers = [
            Router(router_id=i, latency=router_latency)
            for i in range(num_routers)
        ]
        network.routers = routers

        link_count = 0

        # One controller per router; routers beyond len(nodes) are empty.
        ext_links = []
        for i, node in enumerate(nodes):
            ext_links.append(
                ExtLink(
                    link_id=link_count,
                    ext_node=node,
                    int_node=routers[i],
                    latency=link_latency,
                )
            )
            link_count += 1

        network.ext_links = ext_links

        int_links = []

        # East output to West input (weight=1)
        for row in range(rows):
            for col in range(cols):
                if col + 1 < cols:
                    src = col + row * cols
                    dst = (col + 1) + row * cols
                    int_links.append(
                        IntLink(
                            link_id=link_count,
                            src_node=routers[src],
                            dst_node=routers[dst],
                            src_outport="East",
                            dst_inport="West",
                            latency=link_latency,
                            weight=1,
                        )
                    )
                    link_count += 1

        # West output to East input (weight=1)
        for row in range(rows):
            for col in range(cols):
                if col + 1 < cols:
                    src = (col + 1) + row * cols
                    dst = col + row * cols
                    int_links.append(
                        IntLink(
                            link_id=link_count,
                            src_node=routers[src],
                            dst_node=routers[dst],
                            src_outport="West",
                            dst_inport="East",
                            latency=link_latency,
                            weight=1,
                        )
                    )
                    link_count += 1

        # North output to South input (weight=2)
        for col in range(cols):
            for row in range(rows):
                if row + 1 < rows:
                    src = col + row * cols
                    dst = col + (row + 1) * cols
                    int_links.append(
                        IntLink(
                            link_id=link_count,
                            src_node=routers[src],
                            dst_node=routers[dst],
                            src_outport="North",
                            dst_inport="South",
                            latency=link_latency,
                            weight=2,
                        )
                    )
                    link_count += 1

        # South output to North input (weight=2)
        for col in range(cols):
            for row in range(rows):
                if row + 1 < rows:
                    src = col + (row + 1) * cols
                    dst = col + row * cols
                    int_links.append(
                        IntLink(
                            link_id=link_count,
                            src_node=routers[src],
                            dst_node=routers[dst],
                            src_outport="South",
                            dst_inport="North",
                            latency=link_latency,
                            weight=2,
                        )
                    )
                    link_count += 1

        network.int_links = int_links
