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

#ifndef SRC_DCP_CONSUMER_H_
#define SRC_DCP_CONSUMER_H_ 1

#include "config.h"

#include "atomic_unordered_map.h"

#include "connhandler.h"
#include "dcp/flow-control.h"

#include <relaxed_atomic.h>

class DcpResponse;
class StreamEndResponse;

class DcpConsumer : public ConnHandler,
                    public std::enable_shared_from_this<DcpConsumer> {
    typedef std::map<uint32_t, std::pair<uint32_t, uint16_t>> opaque_map;

public:

    DcpConsumer(EventuallyPersistentEngine &e, const void *cookie,
                const std::string &name);

    virtual ~DcpConsumer();

    const char *getType() const override { return "consumer"; };

    /*
     * Creates a PassiveStream.
     *
     * @param e Reference to the engine
     * @param consumer The consumer the new stream will belong to
     * @param name The name of the new stream
     * @param flags The DCP flags
     * @param opaque The stream opaque
     * @param vb The vbucket the stream belongs to
     * @param start_seqno The start sequence number of the stream
     * @param end_seqno The end sequence number of the stream
     * @param vb_uuid The uuid of the vbucket the stream belongs to
     * @param snap_start_seqno The snapshot start sequence number
     * @param snap_end_seqno The snapshot end sequence number
     * @param vb_high_seqno The last received sequence number
     *
     * @return a SingleThreadedRCPtr to the newly created PassiveStream.
     */
    virtual std::shared_ptr<PassiveStream> makePassiveStream(
            EventuallyPersistentEngine& e,
            std::shared_ptr<DcpConsumer> consumer,
            const std::string& name,
            uint32_t flags,
            uint32_t opaque,
            uint16_t vb,
            uint64_t start_seqno,
            uint64_t end_seqno,
            uint64_t vb_uuid,
            uint64_t snap_start_seqno,
            uint64_t snap_end_seqno,
            uint64_t vb_high_seqno);

    ENGINE_ERROR_CODE addStream(uint32_t opaque, uint16_t vbucket,
                                uint32_t flags) override;

    ENGINE_ERROR_CODE closeStream(uint32_t opaque, uint16_t vbucket) override;

    ENGINE_ERROR_CODE streamEnd(uint32_t opaque, uint16_t vbucket,
                                uint32_t flags) override;

    ENGINE_ERROR_CODE mutation(uint32_t opaque,
                               const DocKey& key,
                               cb::const_byte_buffer value,
                               size_t priv_bytes,
                               uint8_t datatype,
                               uint64_t cas,
                               uint16_t vbucket,
                               uint32_t flags,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               uint32_t expiration,
                               uint32_t lock_time,
                               cb::const_byte_buffer meta,
                               uint8_t nru) override;

    ENGINE_ERROR_CODE deletion(uint32_t opaque,
                               const DocKey& key,
                               cb::const_byte_buffer value,
                               size_t priv_bytes,
                               uint8_t datatype,
                               uint64_t cas,
                               uint16_t vbucket,
                               uint64_t by_seqno,
                               uint64_t rev_seqno,
                               cb::const_byte_buffer meta) override;

    ENGINE_ERROR_CODE expiration(uint32_t opaque,
                                 const DocKey& key,
                                 cb::const_byte_buffer value,
                                 size_t priv_bytes,
                                 uint8_t datatype,
                                 uint64_t cas,
                                 uint16_t vbucket,
                                 uint64_t by_seqno,
                                 uint64_t rev_seqno,
                                 cb::const_byte_buffer meta) override;

    ENGINE_ERROR_CODE snapshotMarker(uint32_t opaque,
                                     uint16_t vbucket,
                                     uint64_t start_seqno,
                                     uint64_t end_seqno,
                                     uint32_t flags) override;

    ENGINE_ERROR_CODE noop(uint32_t opaque) override;

    ENGINE_ERROR_CODE flush(uint32_t opaque, uint16_t vbucket);

    ENGINE_ERROR_CODE setVBucketState(uint32_t opaque, uint16_t vbucket,
                                      vbucket_state_t state) override;

    ENGINE_ERROR_CODE step(struct dcp_message_producers* producers) override;

    /**
     * Sub-classes must implement a method that processes a response
     * to a request initiated by itself.
     *
     * @param resp A mcbp response message to process.
     * @returns true/false which will be converted to SUCCESS/DISCONNECT by the
     *          engine.
     */
    bool handleResponse(const protocol_binary_response_header* resp) override;

    /**
     * Push a systemEvent onto this DCP consumer for consumption by a VB
     *
     * @param opaque The opaque for the stream.
     * @param vbucket The vbucket the event is being sent to.
     * @param event The mcbp::systemevent::id value.
     * @param bySeqno The seqno of the event.
     * @param key The event's key.
     * @param eventData The event's specific data.
     */
    ENGINE_ERROR_CODE systemEvent(uint32_t opaque,
                                  uint16_t vbucket,
                                  mcbp::systemevent::id event,
                                  uint64_t bySeqno,
                                  cb::const_byte_buffer key,
                                  cb::const_byte_buffer eventData) override;

    bool doRollback(uint32_t opaque, uint16_t vbid, uint64_t rollbackSeqno);

    void addStats(ADD_STAT add_stat, const void *c) override;

    void aggregateQueueStats(ConnCounter& aggregator) override;

    void notifyStreamReady(uint16_t vbucket);

    void closeAllStreams();

    void closeStreamDueToVbStateChange(uint16_t vbucket, vbucket_state_t state);

    process_items_error_t processBufferedItems();

    uint64_t incrOpaqueCounter();

    uint32_t getFlowControlBufSize();

    void setFlowControlBufSize(uint32_t newSize);

    static const std::string& getControlMsgKey(void);

    bool isStreamPresent(uint16_t vbucket);

    void cancelTask();

    void taskCancelled();

    bool notifiedProcessor(bool to);

    void setProcessorTaskState(enum process_items_error_t to);

    std::string getProcessorTaskStatusStr();

    /**
     * Check if the enough bytes have been removed from the
     * flow control buffer, for the consumer to send an ACK
     * back to the producer.
     *
     * @param schedule true if the notification is to be
     *                 scheduled
     */
    void notifyConsumerIfNecessary(bool schedule);

    void setProcessorYieldThreshold(size_t newValue) {
        processBufferedMessagesYieldThreshold = newValue;
    }

    void setProcessBufferedMessagesBatchSize(size_t newValue) {
        processBufferedMessagesBatchSize = newValue;
    }

    /* Notifies the frontend that this (paused) connection should be
     * re-considered for work.
     * @param schedule If true, schedule the notification on a background
     *                 thread for the ConnNotifier to pick, else notify
     *                 synchronously on this thread.
     */
    void notifyPaused(bool schedule);

    void setDisconnect() override;

protected:
    /**
     * Records when the consumer last received a message from producer.
     * It is used to detect dead connections. The connection is closed
     * if a message, including a No-Op message, is not seen in a
     * specified time period.
     * It is protected so we can access from MockDcpConsumer, for
     * for testing purposes.
     */
    rel_time_t lastMessageTime;

    // Searches the streams map for a stream for vbucket ID. Returns the found
    // stream, or an empty pointer if none found.
    std::shared_ptr<PassiveStream> findStream(uint16_t vbid);

    std::unique_ptr<DcpResponse> getNextItem();

    /**
     * Check if the provided opaque id is one of the
     * current open "session" id's
     *
     * @param opaque the provided opaque
     * @param vbucket the provided vbucket
     * @return true if the session is open, false otherwise
     */
    bool isValidOpaque(uint32_t opaque, uint16_t vbucket);

    void streamAccepted(uint32_t opaque,
                        uint16_t status,
                        const uint8_t* body,
                        uint32_t bodylen);

    ENGINE_ERROR_CODE handleNoop(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE handlePriority(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE handleExtMetaData(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE handleValueCompression(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE supportCursorDropping(struct dcp_message_producers* producers);

    ENGINE_ERROR_CODE sendStreamEndOnClientStreamClose(
            struct dcp_message_producers* producers);

    void notifyVbucketReady(uint16_t vbucket);

    /**
     * Drain the stream of bufferedItems
     * The function will stop draining
     *  - if there's no more data - all_processed
     *  - if the replication throttle says no more - cannot_process
     *  - if there's an error, e.g. ETMPFAIL/ENOMEM - cannot_process
     *  - if we hit the yieldThreshold - more_to_process
     */
    process_items_error_t drainStreamsBufferedItems(
            std::shared_ptr<PassiveStream> stream, size_t yieldThreshold);

    /**
     * This function is called when an addStream command gets a rollback
     * error from the producer.
     *
     * The function will either trigger a rollback to rollbackSeqno or
     * trigger the request of a new stream using the next (older) failover table
     * entry.
     *
     * @param vbid The vbucket the response is for.
     * @param opaque Unique handle for the stream's request/response.
     * @param rollbackSeqno The seqno to rollback to.
     *
     * @returns true/false which will be converted to SUCCESS/DISCONNECT by the
     *          engine.
     */
    bool handleRollbackResponse(uint16_t vbid,
                                uint32_t opaque,
                                uint64_t rollbackSeqno);

    /* Reference to the ep engine; need to create the 'Processor' task */
    EventuallyPersistentEngine& engine;
    uint64_t opaqueCounter;
    size_t processorTaskId;
    std::atomic<enum process_items_error_t> processorTaskState;

    DcpReadyQueue vbReady;
    std::atomic<bool> processorNotification;

    std::mutex readyMutex;
    std::list<uint16_t> ready;

    // Map of vbid -> passive stream. Map itself is atomic (thread-safe).
    typedef AtomicUnorderedMap<uint16_t, std::shared_ptr<PassiveStream>>
            PassiveStreamMap;
    PassiveStreamMap streams;

    /*
     * Each time a stream is added an entry is made into the opaqueMap, which
     * maps a local opaque to a tuple of an externally provided opaque and vbid.
     */
    opaque_map opaqueMap_;

    Couchbase::RelaxedAtomic<uint32_t> backoffs;
    // The maximum interval between dcp messages before the consumer disconnects
    const std::chrono::seconds dcpIdleTimeout;
    // The interval that the consumer tells the producer to send noops
    const std::chrono::seconds dcpNoopTxInterval;

    bool pendingEnableNoop;
    bool pendingSendNoopInterval;
    bool pendingSetPriority;
    bool pendingEnableExtMetaData;
    bool pendingEnableValueCompression;
    bool pendingSupportCursorDropping;
    bool pendingSendStreamEndOnClientStreamClose;

    /* Indicates if the 'Processor' task is running */
    std::atomic<bool> processorTaskRunning;

    FlowControl flowControl;

       /**
     * An upper bound on how many times drainStreamsBufferedItems will
     * call into processBufferedMessages before returning and triggering
     * Processor to yield. Initialised from the configuration
     *  'dcp_consumer_process_buffered_messages_yield_limit'
     */
    size_t processBufferedMessagesYieldThreshold;

    /**
     * An upper bound on how many items a single consumer stream will process
     * in one call of stream->processBufferedMessages()
     */
    size_t processBufferedMessagesBatchSize;

    static const std::string noopCtrlMsg;
    static const std::string noopIntervalCtrlMsg;
    static const std::string connBufferCtrlMsg;
    static const std::string priorityCtrlMsg;
    static const std::string extMetadataCtrlMsg;
    static const std::string valueCompressionCtrlMsg;
    static const std::string cursorDroppingCtrlMsg;
    static const std::string sendStreamEndOnClientStreamCloseCtrlMsg;
};

/*
 * Task that orchestrates rollback on Consumer,
 * runs in background.
 */
class RollbackTask : public GlobalTask {
public:
    RollbackTask(EventuallyPersistentEngine* e,
                 uint32_t opaque_,
                 uint16_t vbid_,
                 uint64_t rollbackSeqno_,
                 std::shared_ptr<DcpConsumer> conn)
        : GlobalTask(e, TaskId::RollbackTask, 0, false),
          description("Running rollback task for vbucket " +
                      std::to_string(vbid_)),
          engine(e),
          opaque(opaque_),
          vbid(vbid_),
          rollbackSeqno(rollbackSeqno_),
          cons(conn) {
    }

    cb::const_char_buffer getDescription() {
        return description;
    }

    std::chrono::microseconds maxExpectedDuration() {
        // Little data on how long this typically takes (rare operation).
        // Somewhat arbitrary selection of 10s as being slow.
        return std::chrono::seconds(10);
    }

    bool run();

private:
    const std::string description;
    EventuallyPersistentEngine *engine;
    uint32_t opaque;
    uint16_t vbid;
    uint64_t rollbackSeqno;
    std::shared_ptr<DcpConsumer> cons;
};

#endif  // SRC_DCP_CONSUMER_H_
