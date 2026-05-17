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
CHI C2C protocol flow integration tests.

Exercises cross-chip coherent traffic through the C2C gateway:
- SeriesGetx: all-store traffic (write flows)
- SeriesGets: all-load traffic (read flows)
- SeriesGetMixed: mixed read/write traffic

Each test uses a two-chip CHI topology with packetizer bridges
and deferred credit returns.  Tests pass if gem5 exits cleanly
(no panic, no assert, exit code 0).
"""

import re

from testlib import *

config = joinpath(absdirpath(__file__), "configs", "chi_c2c_run.py")
ruby_mem_config = joinpath(
    absdirpath(__file__), "configs", "chi_c2c_ruby_mem_test.py"
)


def limit_reached_verifier(tick):
    return verifier.MatchRegex(
        re.compile(
            f"Exiting @ tick {tick} because simulate\\(\\) limit reached"
        )
    )


c2c_tests = [
    ("chi-c2c-series-getx", "SeriesGetx", 20),
    ("chi-c2c-series-gets", "SeriesGets", 20),
    ("chi-c2c-series-mixed", "SeriesGetMixed", 20),
    ("chi-c2c-invalidate", "Invalidate", 100),
]

# TODO: FR-C04 same-address concurrency test needs RubyDirectedTester
# support for same-address sequences

for name, test_type, requests in c2c_tests:
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(),
        config=config,
        config_args=[
            "--test-type",
            test_type,
            "--requests",
            str(requests),
            "--topology=Crossbar",
        ],
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        protocol="CHI",
        length=constants.long_tag,
    )


cross_snf_tests = [
    (
        "chi-c2c-cross-snf-dmt",
        "SeriesGets",
        40,
        ["--cross-snf", "--enable-dmt-early-dealloc"],
    ),
    ("chi-c2c-cross-snf-dwt", "SeriesGetx", 40, ["--cross-snf"]),
]

for name, test_type, requests, extra_args in cross_snf_tests:
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(),
        config=config,
        config_args=[
            "--test-type",
            test_type,
            "--requests",
            str(requests),
            "--topology=Crossbar",
        ]
        + extra_args,
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        protocol="CHI",
        length=constants.long_tag,
    )


ruby_mem_long_tests = [
    (
        "chi-c2c-ruby-mem-r2-long",
        [
            "--topology",
            "R2",
            "--num-cpus",
            "4",
            "--progress",
            "500000",
            "--abs-max-tick",
            "10000000000",
        ],
    ),
    (
        "chi-c2c-ruby-mem-r3-long",
        [
            "--topology",
            "R3",
            "--num-cpus",
            "4",
            "--num-c2cgs",
            "3",
            "--progress",
            "500000",
            "--abs-max-tick",
            "10000000000",
        ],
    ),
]

for name, config_args in ruby_mem_long_tests:
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=[limit_reached_verifier(10000000000)],
        config=ruby_mem_config,
        config_args=config_args,
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        protocol="CHI",
        length=constants.very_long_tag,
    )


atomic_forward_tests = [
    (
        "chi-c2c-ruby-mem-r2-atomic-forward",
        [
            "--topology",
            "R2",
            "--num-cpus",
            "4",
            "--atomic",
            "100",
            "--progress",
            "50000",
            "--abs-max-tick",
            "1000000",
        ],
    ),
    (
        "chi-c2c-ruby-mem-r3-atomic-forward",
        [
            "--topology",
            "R3",
            "--num-cpus",
            "4",
            "--num-c2cgs",
            "3",
            "--atomic",
            "100",
            "--progress",
            "50000",
            "--abs-max-tick",
            "1000000",
        ],
    ),
]

for name, config_args in atomic_forward_tests:
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=[limit_reached_verifier(1000000)],
        config=ruby_mem_config,
        config_args=config_args,
        valid_isas=(constants.all_compiled_tag,),
        valid_hosts=constants.supported_hosts,
        protocol="CHI",
        length=constants.long_tag,
    )
