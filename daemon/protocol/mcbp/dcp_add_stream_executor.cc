/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <daemon/mcbp.h>
#include "executors.h"

void dcp_add_stream_executor(Cookie& cookie) {
    auto packet = cookie.getPacket(Cookie::PacketContent::Full);
    const auto* req =
            reinterpret_cast<const protocol_binary_request_dcp_add_stream*>(
                    packet.data());

    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);
    uint32_t flags = ntohl(req->message.body.flags);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        ret = connection.getBucketEngine()->dcp.add_stream(
                connection.getBucketEngineAsV0(),
                &cookie,
                req->message.header.request.opaque,
                ntohs(req->message.header.request.vbucket),
                flags);
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        connection.setDCP(true);
        connection.setState(McbpStateMachine::State::ship_log);
        break;
    case ENGINE_DISCONNECT:
        connection.setState(McbpStateMachine::State::closing);
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
