/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace rtengine::stage
{

/*
 * One adapter's block-id-to-handler mapping.
 *
 * The registry is the executor's side of the stage contract. A path adapter
 * builds one registry, plan validation checks every non-folded contract node
 * against it, and execution looks the handler up in the same registry. The
 * adapter therefore has one id list, not a coverage list plus a dispatch
 * switch.
 *
 * The handler type is a template parameter: a member-function pointer where
 * the bodies share a signature, or a callable wrapper where they do not.
 */
template <typename Handler>
class StageHandlerRegistry
{
public:
    void add(const char* id, Handler handler)
    {
        assert(id != nullptr && id[0] != '\0');
        for (const auto& entry : entries_) {
            (void)entry;
            assert(std::string(id) != entry.first &&
                   "a handler id is registered twice");
        }
        entries_.emplace_back(id, std::move(handler));
    }

    Handler find(const std::string& id) const
    {
        for (const auto& entry : entries_) {
            if (id == entry.first) {
                return entry.second;
            }
        }
        return Handler{};
    }

    bool contains(const std::string& id) const
    {
        for (const auto& entry : entries_) {
            if (id == entry.first) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> ids() const
    {
        std::vector<std::string> result;
        result.reserve(entries_.size());
        for (const auto& entry : entries_) {
            result.push_back(entry.first);
        }
        return result;
    }

    const std::vector<std::pair<std::string, Handler>>& entries() const
    {
        return entries_;
    }

private:
    std::vector<std::pair<std::string, Handler>> entries_;
};

} // namespace rtengine::stage
