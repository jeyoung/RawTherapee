/* -*- C++ -*-
 *  
 *  This file is part of RawTherapee.
 *
 *  Copyright (c) 2017 Alberto Griggio <alberto.griggio@gmail.com>
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

#include <string>
#include <map>
#include <unordered_map>
#include "rtengine/editorrequest.h"
#include "rtengine/editorrequest_registry.h"


class ProcEventMapper {
public:
    static ProcEventMapper *getInstance();
    /*
     * Typed producer entry point. The tool names what it asks for; the
     * request kind and facts are built in editorrequest.cc. This is the form
     * converted tools use.
     */
    rtengine::ProcEvent newEvent(rtengine::RequestIntent intent,
                                 const std::string &history_msg="");
    rtengine::ProcEvent newEvent(rtengine::EditorRequest request,
                                 const std::string &history_msg="");
    const std::string &getHistoryMsg(rtengine::ProcEvent event) const;

private:
    ProcEventMapper();
    
    std::unordered_map<int, std::string> history_msgs_;
    std::map<std::string, rtengine::ProcEvent> events_;
};
