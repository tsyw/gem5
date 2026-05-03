/*
 * Copyright (c) 2026 The gem5 Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_RUBY_PROTOCOL_REMOTESHARERMAP_HH__
#define __MEM_RUBY_PROTOCOL_REMOTESHARERMAP_HH__

#include <cassert>
#include <ostream>
#include <unordered_map>

#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/common/NetDest.hh"

namespace gem5
{

namespace ruby
{

class RemoteSharerMap
{
  private:
    std::unordered_map<MachineID, MachineID> sharerToGateway;

    template <typename Func>
    void
    forEachSharer(const NetDest &sharers, Func func) const
    {
        NetDest remaining = sharers;
        while (!remaining.isEmpty()) {
            const MachineID sharer = remaining.smallestElement();
            remaining.remove(sharer);
            func(sharer);
        }
    }

  public:
    void
    clear()
    {
        sharerToGateway.clear();
    }

    void
    add(MachineID sharer, MachineID gateway)
    {
        sharerToGateway[sharer] = gateway;
    }

    void
    addNetDest(const NetDest &sharers, MachineID gateway)
    {
        forEachSharer(sharers,
                      [&](MachineID sharer) { add(sharer, gateway); });
    }

    bool
    contains(MachineID sharer) const
    {
        return sharerToGateway.find(sharer) != sharerToGateway.end();
    }

    MachineID
    lookup(MachineID sharer) const
    {
        const auto it = sharerToGateway.find(sharer);
        assert(it != sharerToGateway.end());
        return it->second;
    }

    void
    remove(MachineID sharer)
    {
        sharerToGateway.erase(sharer);
    }

    void
    removeNetDest(const NetDest &sharers)
    {
        forEachSharer(sharers, [&](MachineID sharer) { remove(sharer); });
    }

    void
    removeByGateway(MachineID gateway)
    {
        for (auto it = sharerToGateway.begin(); it != sharerToGateway.end();) {
            if (it->second == gateway) {
                it = sharerToGateway.erase(it);
            } else {
                ++it;
            }
        }
    }

    int
    countGatewayUsers(MachineID gateway) const
    {
        int count = 0;
        for (const auto &[_, mappedGateway] : sharerToGateway) {
            if (mappedGateway == gateway) {
                ++count;
            }
        }
        return count;
    }

    NetDest
    remoteSharersIn(const NetDest &candidates) const
    {
        NetDest sharers = candidates;
        sharers.clear();
        forEachSharer(candidates, [&](MachineID candidate) {
            if (contains(candidate)) {
                sharers.add(candidate);
            }
        });
        return sharers;
    }

    NetDest
    gatewaysFor(const NetDest &sharers) const
    {
        NetDest gateways = sharers;
        gateways.clear();
        forEachSharer(sharers, [&](MachineID sharer) {
            if (contains(sharer)) {
                gateways.add(lookup(sharer));
            }
        });
        return gateways;
    }

    NetDest
    sharersForGateway(const NetDest &prototype, MachineID gateway) const
    {
        NetDest sharers = prototype;
        sharers.clear();
        for (const auto &[sharer, mappedGateway] : sharerToGateway) {
            if (mappedGateway == gateway) {
                sharers.add(sharer);
            }
        }
        return sharers;
    }

    void
    print(std::ostream &out) const
    {
        out << "{";
        bool first = true;
        for (const auto &[sharer, gateway] : sharerToGateway) {
            if (!first) {
                out << ", ";
            }
            out << sharer << "->" << gateway;
            first = false;
        }
        out << "}";
    }
};

inline std::ostream &
operator<<(std::ostream &out, const RemoteSharerMap &obj)
{
    obj.print(out);
    out << std::flush;
    return out;
}

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_REMOTESHARERMAP_HH__
