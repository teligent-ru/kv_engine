/*
 *     Copyright 2014 Couchbase, Inc.
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

#ifndef SRC_EXECUTORPOOL_H_
#define SRC_EXECUTORPOOL_H_ 1

#include "config.h"

#include <map>
#include <queue>

#include "tasks.h"
#include "ringbuffer.h"
#include "task_type.h"

// Forward decl
class TaskQueue;
class ExecutorThread;
class TaskLogEntry;

typedef std::vector<ExecutorThread *> ThreadQ;
typedef std::pair<ExTask, TaskQueue *> TaskQpair;
typedef std::pair<RingBuffer<TaskLogEntry>*, RingBuffer<TaskLogEntry> *>
                                                                TaskLog;
typedef std::vector<TaskQueue *> TaskQ;

class ExecutorPool {
public:

    void addWork(size_t newWork);

    void wakeMore(size_t numToWake, TaskQueue *curQ);

    void lessWork(void);

    void doneWork(task_type_t &doneTaskType);

    task_type_t tryNewWork(task_type_t newTaskType);

    bool trySleep(void) {
        if (!numReadyTasks) {
            numSleepers++;
            return true;
        }
        return false;
    }

    void woke(void) {
        numSleepers--;
    }

    TaskQueue *nextTask(ExecutorThread &t, uint8_t tick);

    TaskQueue *getSleepQ(unsigned int curTaskType) {
        return isHiPrioQset ? hpTaskQ[curTaskType] : lpTaskQ[curTaskType];
    }

    bool cancel(size_t taskId, bool eraseTask=false);

    bool stopTaskGroup(EventuallyPersistentEngine *e, task_type_t qidx);

    bool wake(size_t taskId);

    bool snooze(size_t taskId, double tosleep);

    void registerBucket(EventuallyPersistentEngine *engine);

    void unregisterBucket(EventuallyPersistentEngine *engine);

    void doWorkerStat(EventuallyPersistentEngine *engine, const void *cookie,
                      ADD_STAT add_stat);

    void doTaskQStat(EventuallyPersistentEngine *engine, const void *cookie,
                     ADD_STAT add_stat);

    size_t getNumWorkersStat(void) { return threadQ.size(); }

    size_t getNumCPU(void);

    size_t getNumWorkers(void);

    size_t getNumReaders(void);

    size_t getNumWriters(void);

    size_t getNumAuxIO(void);

    size_t getNumNonIO(void);

    size_t getNumReadyTasks(void) { return numReadyTasks; }

    size_t getNumSleepers(void) { return numSleepers; }

    size_t schedule(ExTask task, task_type_t qidx);

    static ExecutorPool *get(void);

private:

    ExecutorPool(size_t m, size_t nTaskSets);
    ~ExecutorPool(void);

    TaskQueue* _nextTask(ExecutorThread &t, uint8_t tick);
    bool _cancel(size_t taskId, bool eraseTask=false);
    bool _wake(size_t taskId);
    bool _startWorkers(void);
    bool _snooze(size_t taskId, double tosleep);
    size_t _schedule(ExTask task, task_type_t qidx);
    void _registerBucket(EventuallyPersistentEngine *engine);
    void _unregisterBucket(EventuallyPersistentEngine *engine);
    bool _stopTaskGroup(EventuallyPersistentEngine *e, task_type_t qidx);
    TaskQueue* _getTaskQueue(EventuallyPersistentEngine *e, task_type_t qidx);

    size_t maxGlobalThreads;
    size_t numTaskSets; // safe to read lock-less not altered after creation

    AtomicValue<size_t> numReadyTasks;
    SyncObject mutex; // Thread management condition var + mutex

    //! A mapping of task ids to Task, TaskQ in the thread pool
    std::map<size_t, TaskQpair> taskLocator;

    //A list of threads
    ThreadQ threadQ;

    // Global cross bucket priority queues where tasks get scheduled into ...
    TaskQ hpTaskQ; // a vector array of numTaskSets elements for high priority
    bool isHiPrioQset;

    TaskQ lpTaskQ; // a vector array of numTaskSets elements for low priority
    bool isLowPrioQset;

    size_t numBuckets;

    SyncObject tMutex; // to serialize taskLocator, threadQ, numBuckets access

    AtomicValue<uint16_t> numSleepers; // total number of sleeping threads
    AtomicValue<uint16_t> *curWorkers; // track # of active workers per TaskSet
    uint16_t *maxWorkers; // and limit it to the value set here

    // Singleton creation
    static Mutex initGuard;
    static ExecutorPool *instance;
};
#endif  // SRC_EXECUTORPOOL_H_
