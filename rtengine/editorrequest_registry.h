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

#include "editorrequest.h"
#include "procevents.h"

#include <unordered_map>

namespace rtengine
{

/*
 * Event-to-request registry.
 *
 * A ProcEvent is the identity and history token of one producer action; it is
 * not a request. The registry stores the typed request an action registered,
 * allocates fresh identities for typed producers, and resolves the recorded
 * request classification of a static ProcEventCode.
 *
 * The static classification is a range table of typed intents, one row per
 * contiguous run of events with the same request families. It replaces the
 * retired action table and carries no integer mask.
 */
class EditorRequestRegistry {
public:
    static EditorRequestRegistry *getInstance();

    /* Allocate a fresh dynamic event identity. */
    ProcEvent newEvent();

    /* Register the typed request of a dynamic producer event. */
    void registerRequest(ProcEvent event, const EditorRequest& request);

    /*
     * Resolve the typed request of an event.
     *
     * A dynamic event returns the request it registered. A static
     * ProcEventCode returns the request of its recorded classification. An
     * unknown event is a contract violation and fails fast.
     */
    EditorRequest requestFor(ProcEvent event) const;

    /* Whether the event has a registered dynamic request. */
    bool hasRegisteredRequest(ProcEvent event) const;

    /*
     * Recorded classification of a static ProcEventCode. This is the table
     * lookup the registry uses for static identities; a code outside the
     * static range is a contract violation and fails fast.
     */
    static RequestIntent staticEventIntent(int code);

private:
    EditorRequestRegistry();

    int nextEvent_;
    std::unordered_map<int, EditorRequest> requests_;
};

} // namespace rtengine
