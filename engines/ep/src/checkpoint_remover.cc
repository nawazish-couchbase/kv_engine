/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "checkpoint_remover.h"
#include "bucket_logger.h"
#include "checkpoint_config.h"
#include "checkpoint_manager.h"
#include "checkpoint_visitor.h"
#include "connmap.h"
#include "dcp/dcpconnmap.h"
#include "ep_engine.h"
#include "kv_bucket.h"
#include <executor/executorpool.h>

#include <phosphor/phosphor.h>
#include <limits>
#include <memory>

CheckpointDestroyerTask::CheckpointDestroyerTask(EventuallyPersistentEngine* e)
    : GlobalTask(e,
                 TaskId::CheckpointDestroyerTask,
                 std::numeric_limits<int>::max() /* sleepTime */) {
}

bool CheckpointDestroyerTask::run() {
    if (engine->getEpStats().isShutdown) {
        return false;
    }
    // sleep forever once done, until notified again
    snooze(std::numeric_limits<int>::max());
    notified.store(false);
    // to hold the lock for as short of a time as possible, swap toDestroy
    // with a temporary list, and destroy the temporary list outside of the lock
    CheckpointList temporary;
    {
        auto handle = toDestroy.lock();
        handle->swap(temporary);
    }
    return true;
}

void CheckpointDestroyerTask::queueForDestruction(CheckpointList&& list) {
    // iterating the list is not ideal, but it should generally be
    // small (in many cases containing a single item), and correctly tracking
    // memory usage is useful.
    for (auto& checkpoint : list) {
        checkpoint->setMemoryTracker(&pendingDestructionMemoryUsage);
    }
    {
        auto handle = toDestroy.lock();
        handle->splice(handle->end(), list);
    }
    bool expected = false;
    if (notified.compare_exchange_strong(expected, true)) {
        ExecutorPool::get()->wake(getId());
    }
}

size_t CheckpointDestroyerTask::getMemoryUsage() const {
    return pendingDestructionMemoryUsage.load();
}

ClosedUnrefCheckpointRemoverTask::ClosedUnrefCheckpointRemoverTask(
        EventuallyPersistentEngine* e, EPStats& st, size_t interval)
    : GlobalTask(e, TaskId::ClosedUnrefCheckpointRemoverTask, interval, false),
      engine(e),
      stats(st),
      sleepTime(interval),
      available(true),
      shouldScanForUnreferencedCheckpoints(
              !e->getCheckpointConfig().isEagerCheckpointRemoval()) {
}

size_t ClosedUnrefCheckpointRemoverTask::attemptCheckpointRemoval(
        size_t memToClear) {
    size_t memoryCleared = 0;
    auto& bucket = *engine->getKVBucket();
    const auto vbuckets = bucket.getVBuckets().getVBucketsSortedByChkMgrMem();
    for (const auto& it : vbuckets) {
        if (memoryCleared >= memToClear) {
            break;
        }

        auto vb = bucket.getVBucket(it.first);
        if (!vb) {
            continue;
        }

        memoryCleared +=
                vb->checkpointManager->removeClosedUnrefCheckpoints().memory;
    }
    return memoryCleared;
}

size_t ClosedUnrefCheckpointRemoverTask::attemptItemExpelling(
        size_t memToClear) {
    size_t memoryCleared = 0;
    auto& kvBucket = *engine->getKVBucket();
    const auto vbuckets = kvBucket.getVBuckets().getVBucketsSortedByChkMgrMem();
    for (const auto& it : vbuckets) {
        if (memoryCleared >= memToClear) {
            break;
        }
        const auto vbid = it.first;
        VBucketPtr vb = kvBucket.getVBucket(vbid);
        if (!vb) {
            continue;
        }

        const auto expelResult =
                vb->checkpointManager->expelUnreferencedCheckpointItems();
        EP_LOG_DEBUG(
                "Expelled {} unreferenced checkpoint items "
                "from {} "
                "and estimated to have recovered {} bytes.",
                expelResult.count,
                vb->getId(),
                expelResult.memory);
        memoryCleared += expelResult.memory;
    }
    return memoryCleared;
}

bool ClosedUnrefCheckpointRemoverTask::run() {
    TRACE_EVENT0("ep-engine/task", "ClosedUnrefCheckpointRemoverTask");

    bool inverse = true;
    if (shouldScanForUnreferencedCheckpoints &&
        !available.compare_exchange_strong(inverse, false)) {
        snooze(sleepTime);
        return true;
    }

    auto* kvBucket = engine->getKVBucket();
    const auto memToClear = kvBucket->getRequiredCheckpointMemoryReduction();

    if (memToClear == 0) {
        available = true;
        snooze(sleepTime);
        return true;
    }

    size_t memRecovered{0};

    // Try full CheckpointRemoval first, across all vbuckets
    if (shouldScanForUnreferencedCheckpoints) {
        memRecovered += attemptCheckpointRemoval(memToClear);
    } else {
#if CB_DEVELOPMENT_ASSERTS
        // if eager checkpoint removal has been configured, calling
        // attemptCheckpointRemoval here should never, ever, find any
        // checkpoints to remove; they should always be removed as soon
        // as they are made eligible, before the lock is released.
        // This is not cheap to verify, as it requires scanning every
        // vbucket, so is only checked if dev asserts are on.
        Expects(attemptCheckpointRemoval(memToClear) == 0);
#endif
    }
    if (memRecovered >= memToClear) {
        // Recovered enough by CheckpointRemoval, done
        available = true;
        snooze(sleepTime);
        return true;
    }

    // Try expelling, if enabled.
    // Note: The next call tries to expel from all vbuckets before returning.
    // The reason behind trying expel here is to avoid dropping cursors if
    // possible, as that kicks the stream back to backfilling.
    if (engine->getConfiguration().isChkExpelEnabled()) {
        memRecovered += attemptItemExpelling(memToClear);
    }

    if (memRecovered >= memToClear) {
        // Recovered enough by ItemExpel, done
        available = true;
        snooze(sleepTime);
        return true;
    }

    // More memory to recover, try CursorDrop + CheckpointRemoval
    const auto leftToClear = memToClear - memRecovered;
    auto visitor = std::make_unique<CheckpointVisitor>(
            kvBucket, stats, available, leftToClear);

    // Note: Empirical evidence from perf runs shows that 99.9% of "Checkpoint
    // Remover" task should complete under 50ms.
    //
    // @todo: With changes for MB-48038 we are doing more work in the
    //  CheckpointVisitor, so the expected duration will probably need to be
    //  adjusted.
    kvBucket->visitAsync(std::move(visitor),
                         "Checkpoint Remover",
                         TaskId::ClosedUnrefCheckpointRemoverVisitorTask,
                         std::chrono::milliseconds(50) /*maxExpectedDuration*/);

    snooze(sleepTime);
    return true;
}
