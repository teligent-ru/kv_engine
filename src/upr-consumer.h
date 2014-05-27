/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
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

#ifndef SRC_UPR_CONSUMER_H_
#define SRC_UPR_CONSUMER_H_ 1

#include "config.h"

#include "tapconnection.h"
#include "upr-stream.h"

class PassiveStream;
class UprResponse;

typedef enum {
    all_processed,
    more_to_process,
    cannot_process
} process_items_error_t;

class UprConsumer : public Consumer, public Notifiable {
typedef std::map<uint32_t, std::pair<uint32_t, uint16_t> > opaque_map;
public:

    UprConsumer(EventuallyPersistentEngine &e, const void *cookie,
                const std::string &n);

    ~UprConsumer();

    ENGINE_ERROR_CODE addStream(uint32_t opaque, uint16_t vbucket,
                                uint32_t flags);

    ENGINE_ERROR_CODE closeStream(uint32_t opaque, uint16_t vbucket);

    ENGINE_ERROR_CODE streamEnd(uint32_t opaque, uint16_t vbucket,
                                uint32_t flags);

    ENGINE_ERROR_CODE mutation(uint32_t opaque, const void* key, uint16_t nkey,
                               const void* value, uint32_t nvalue, uint64_t cas,
                               uint16_t vbucket, uint32_t flags,
                               uint8_t datatype, uint32_t locktime,
                               uint64_t bySeqno, uint64_t revSeqno,
                               uint32_t exptime, uint8_t nru, const void* meta,
                               uint16_t nmeta);

    ENGINE_ERROR_CODE deletion(uint32_t opaque, const void* key, uint16_t nkey,
                               uint64_t cas, uint16_t vbucket, uint64_t bySeqno,
                               uint64_t revSeqno, const void* meta,
                               uint16_t nmeta);

    ENGINE_ERROR_CODE expiration(uint32_t opaque, const void* key,
                                 uint16_t nkey, uint64_t cas, uint16_t vbucket,
                                 uint64_t bySeqno, uint64_t revSeqno,
                                 const void* meta, uint16_t nmeta);

    ENGINE_ERROR_CODE snapshotMarker(uint32_t opaque,
                                     uint16_t vbucket,
                                     uint64_t start_seqno,
                                     uint64_t end_seqno,
                                     uint32_t flags);

    ENGINE_ERROR_CODE flush(uint32_t opaque, uint16_t vbucket);

    ENGINE_ERROR_CODE setVBucketState(uint32_t opaque, uint16_t vbucket,
                                      vbucket_state_t state);

    ENGINE_ERROR_CODE step(struct upr_message_producers* producers);

    ENGINE_ERROR_CODE handleResponse(protocol_binary_response_header *resp);

    void doRollback(EventuallyPersistentStore *st, uint32_t opaque,
                    uint16_t vbid, uint64_t rollbackSeqno);

    void addStats(ADD_STAT add_stat, const void *c);

    void notifyStreamReady(uint16_t vbucket);

    process_items_error_t processBufferedItems();

private:

    UprResponse* getNextItem();

    /**
     * Check if the provided opaque id is one of the
     * current open "session" id's
     *
     * @param opaque the provided opaque
     * @param vbucket the provided vbucket
     * @return true if the session is open, false otherwise
     */
    bool isValidOpaque(uint32_t opaque, uint16_t vbucket);

    void streamAccepted(uint32_t opaque, uint16_t status, uint8_t* body,
                        uint32_t bodylen);

    uint64_t opaqueCounter;
    size_t processTaskId;
    AtomicValue<bool> itemsToProcess;
    Mutex streamMutex;
    std::list<uint16_t> ready;
    passive_stream_t* streams;
    opaque_map opaqueMap_;

    struct FlowControl {
        FlowControl() : enabled(true), pendingControl(true), bufferSize(0),
                        maxUnackedBytes(0), freedBytes(0) {}
        bool enabled;
        bool pendingControl;
        uint32_t bufferSize;
        uint32_t maxUnackedBytes;
        AtomicValue<uint32_t> freedBytes;
    } flowControl;
};

/*
 * Task that orchestrates rollback on Consumer,
 * runs in background.
 */
class RollbackTask : public GlobalTask {
public:
    RollbackTask(EventuallyPersistentEngine* e,
                 uint32_t opaque_, uint16_t vbid_,
                 uint64_t rollbackSeqno_, UprConsumer *conn,
                 const Priority &p):
        GlobalTask(e, p, 0, false), engine(e),
        opaque(opaque_), vbid(vbid_), rollbackSeqno(rollbackSeqno_),
        cons(conn) { }

    std::string getDescription() {
        return std::string("Running rollback task for vbucket %d", vbid);
    }

    bool run();

private:
    EventuallyPersistentEngine *engine;
    uint32_t opaque;
    uint16_t vbid;
    uint64_t rollbackSeqno;
    UprConsumer* cons;
};

#endif  // SRC_UPR_CONSUMER_H_
