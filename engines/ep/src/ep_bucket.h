/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
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

#pragma once

#include "kv_bucket.h"
#include "kvstore.h"
#include "utilities/testing_hook.h"

class BgFetcher;
namespace Collections::VB {
class Flush;
}
namespace VB {
class Commit;
}
enum class ValueFilter;
class BucketStatCollector;
class CompactTask;
struct CompactionContext;
struct CompactionStats;

/**
 * Eventually Persistent Bucket
 *
 * A bucket type which stores modifications to disk asynchronously
 * ("eventually").
 * Uses hash partitioning of the keyspace into VBuckets, to support
 * replication, rebalance, failover.
 */
class EPBucket : public KVBucket {
public:
    explicit EPBucket(EventuallyPersistentEngine& theEngine);

    ~EPBucket() override;

    bool initialize() override;

    std::vector<ExTask> deinitialize() override;

    enum class MoreAvailable : uint8_t { No = 0, Yes };
    enum class WakeCkptRemover : uint8_t { No = 0, Yes };

    struct FlushResult {
        FlushResult(MoreAvailable m, size_t n, WakeCkptRemover w)
            : moreAvailable(m), wakeupCkptRemover(w), numFlushed(n) {
        }

        bool operator==(const FlushResult& other) const {
            return (moreAvailable == other.moreAvailable &&
                    numFlushed == other.numFlushed &&
                    wakeupCkptRemover == other.wakeupCkptRemover);
        }

        MoreAvailable moreAvailable = MoreAvailable::No;
        WakeCkptRemover wakeupCkptRemover = WakeCkptRemover::No;
        size_t numFlushed = 0;
    };

    /**
     * Flushes all items waiting for persistence in a given vbucket
     * @param vbid The id of the vbucket to flush
     * @return an instance of FlushResult
     */
    FlushResult flushVBucket(Vbid vbid);
    FlushResult flushVBucket_UNLOCKED(LockedVBucketPtr vb);

    /**
     * Set the number of flusher items which can be included in a
     * single flusher commit. For more details see flusherBatchSplitTrigger
     * description.
     */
    void setFlusherBatchSplitTrigger(size_t limit);

    size_t getFlusherBatchSplitTrigger();

    /**
     * Persist whatever flush-batch previously queued into KVStore.
     *
     * @param vbid
     * @param kvstore
     * @param [out] commitData
     * @return true if flush succeeds, false otherwise
     */
    bool commit(Vbid vbid, KVStore& kvstore, VB::Commit& commitData);

    /// Start the Flusher for all shards in this bucket.
    void startFlusher();

    /// Stop the Flusher for all shards in this bucket.
    void stopFlusher();

    bool pauseFlusher() override;
    bool resumeFlusher() override;

    void wakeUpFlusher() override;

    /**
     * Starts the background fetcher for each shard.
     * @return true if successful.
     */
    bool startBgFetcher();

    /// Stops the background fetcher for each shard.
    void stopBgFetcher();

    /**
     * Schedule compaction with a config -override of KVBucket method
     */
    cb::engine_errc scheduleCompaction(
            Vbid vbid,
            const CompactionConfig& c,
            const void* ck,
            std::chrono::milliseconds delay) override;

    /**
     * Schedule compaction with no config. If a CompactTask is already
     * scheduled then the task will still run, but with whatever config it
     * already has. If a task is already scheduled, the given delay parameter
     * takes effect.
     */
    cb::engine_errc scheduleCompaction(Vbid vbid,
                                       const void* cookie,
                                       std::chrono::milliseconds delay);

    cb::engine_errc cancelCompaction(Vbid vbid) override;

    /**
     * Compaction of a database file
     *
     * @param Vbid vbucket to compact
     * @param config Compaction configuration to use
     * @param cookies used to notify connections of operation completion. This
     *        is non-const as doCompact will update cookies, removing all the
     *        cookies it notified.
     *
     * return true if the compaction needs to be rescheduled and false
     *             otherwise
     */
    bool doCompact(Vbid vbid,
                   CompactionConfig& config,
                   std::vector<const void*>& cookies);

    /**
     * After compaction completes the task can be removed if no further
     * compaction is required. If other compaction tasks exist, one of them
     * will be 'poked' to run. This method is called from CompactTask
     *
     * @param vbid id of vbucket that has completed compaction
     */
    bool updateCompactionTasks(Vbid vbid, bool canErase);

    cb::engine_errc getFileStats(const BucketStatCollector& collector) override;

    cb::engine_errc getPerVBucketDiskStats(const void* cookie,
                                           const AddStatFn& add_stat) override;

    size_t getPageableMemCurrent() const override;
    size_t getPageableMemHighWatermark() const override;
    size_t getPageableMemLowWatermark() const override;

    /**
     * Creates a VBucket object from warmup (can set collection state)
     */
    VBucketPtr makeVBucket(Vbid id,
                           vbucket_state_t state,
                           KVShard* shard,
                           std::unique_ptr<FailoverTable> table,
                           NewSeqnoCallback newSeqnoCb,
                           std::unique_ptr<Collections::VB::Manifest> manifest,
                           vbucket_state_t initState,
                           int64_t lastSeqno,
                           uint64_t lastSnapStart,
                           uint64_t lastSnapEnd,
                           uint64_t purgeSeqno,
                           uint64_t maxCas,
                           int64_t hlcEpochSeqno,
                           bool mightContainXattrs,
                           const nlohmann::json* replicationTopology,
                           uint64_t maxVisibleSeqno) override;

    cb::engine_errc statsVKey(const DocKey& key,
                              Vbid vbucket,
                              const void* cookie) override;

    void completeStatsVKey(const void* cookie,
                           const DocKey& key,
                           Vbid vbid,
                           uint64_t bySeqNum) override;

    RollbackResult doRollback(Vbid vbid, uint64_t rollbackSeqno) override;

    void rollbackUnpersistedItems(VBucket& vb, int64_t rollbackSeqno) override;

    LoadPreparedSyncWritesResult loadPreparedSyncWrites(
            folly::SharedMutex::WriteHolder& vbStateLh, VBucket& vb) override;

    /**
     * Returns the ValueFilter to use for KVStore scans, given the bucket
     * compression mode and (optional) cookie.
     * @param Cookie we are performing the operation for. If non-null, then
     *        acts as an additional constraint on ValueFilter - if cookie
     *        doesn't support Snappy compression then ValueFilter will not
     *        return compressed data.
     */
    ValueFilter getValueFilterForCompressionMode(const void* cookie = nullptr);

    void notifyNewSeqno(const Vbid vbid, const VBNotifyCtx& notifyCtx) override;

    bool isGetAllKeysSupported() const override {
        return true;
    }

    void setRetainErroneousTombstones(bool value) {
        retainErroneousTombstones = value;
    }

    bool isRetainErroneousTombstones() const {
        return retainErroneousTombstones.load();
    }

    Warmup* getWarmup() const override;

    bool isWarmingUp() override;

    bool isWarmupOOMFailure() override;

    bool hasWarmupSetVbucketStateFailed() const override;

    /**
     * This method store the given cookie for later notification iff Warmup has
     * yet to reach and complete the PopulateVBucketMap phase.
     *
     * @param cookie the callers cookie which might be stored for later
     *        notification (see return value)
     * @return true if the cookie was stored for later notification, false if
     *         not.
     */
    bool maybeWaitForVBucketWarmup(const void* cookie) override;

    /**
     * Creates a warmup task if the engine configuration has "warmup=true"
     */
    void initializeWarmupTask();

    /**
     * Starts the warmup task if one is present
     */
    void startWarmupTask();

    bool maybeEnableTraffic();

    void warmupCompleted();

    virtual std::shared_ptr<CompactionContext> makeCompactionContext(
            Vbid vbid, CompactionConfig& config, uint64_t purgeSeqno);

    // implemented by querying StorageProperties for the buckets KVStore
    bool isByIdScanSupported() const override;

    bool canEvictFromReplicas() override {
        return true;
    }

    bool maybeScheduleManifestPersistence(
            const void* cookie,
            std::unique_ptr<Collections::Manifest>& newManifest) override;

    BgFetcher& getBgFetcher(Vbid vbid);

protected:
    // During the warmup phase we might want to enable external traffic
    // at a given point in time.. The LoadStorageKvPairCallback will be
    // triggered whenever we want to check if we could enable traffic..
    friend class LoadStorageKVPairCallback;

    class ValueChangedListener;

    void flushOneDelOrSet(const queued_item& qi, VBucketPtr& vb);

    /**
     * Compaction of a database file
     *
     * @param config the configuration to use for running compaction
     */
    void compactInternal(LockedVBucketPtr& vb, CompactionConfig& config);

    /**
     * Callback to be called on completion of the compaction (just before the
     * atomic switch of the files)
     */
    void compactionCompletionCallback(CompactionContext& ctx);

    /**
     * Update collection state (VB::Manifest) after compaction has completed.
     *
     * @param vb VBucket ref
     * @param stats Map of cid to new size value (new value not delta)
     * @param onDiskDroppedCollectionDataExists true if the compacted file
     *        has dropped collections (documents and/or metadata).
     */
    void updateCollectionStatePostCompaction(
            VBucket& vb,
            CompactionStats::CollectionSizeUpdates& stats,
            bool onDiskDroppedCollectionDataExists);

    void stopWarmup();

    /// function which is passed down to compactor for dropping keys
    virtual void dropKey(Vbid vbid,
                         const DiskDocKey& key,
                         int64_t bySeqno,
                         bool isAbort,
                         int64_t highCompletedSeqno);

    /**
     * Performs operations that must be performed after flush succeeds,
     * regardless of whether we flush non-meta items or a new vbstate only.
     *
     * @param vb
     * @param flushStart Used for updating stats
     * @param itemsFlushed Used for updating stats
     * @param aggStats Used for updating stats
     * @param collectionFlush Used for performing collection-related operations
     */
    void flushSuccessEpilogue(
            VBucket& vb,
            const std::chrono::steady_clock::time_point flushStart,
            size_t itemsFlushed,
            const VBucket::AggregatedFlushStats& aggStats,
            Collections::VB::Flush& collectionFlush);

    /**
     * Performs operations that must be performed after flush fails,
     * regardless of whether we flush non-meta items or a new vbstate only.
     *
     * @param itemsToFlush Used for performing post-flush operations
     */
    void flushFailureEpilogue(VBucket& vb, VBucket::ItemsToFlush& itemsToFlush);

    bool isValidBucketDurabilityLevel(
            cb::durability::Level level) const override;

    /**
     * Setup shards.
     */
    void initializeShards();

    cb::engine_errc scheduleCompaction(Vbid vbid,
                                       std::optional<CompactionConfig> config,
                                       const void* cookie,
                                       std::chrono::milliseconds delay);

    /**
     * Max number of backill items in a single flusher batch before we split
     * into multiple batches. Actual batch size may be larger as we will not
     * split Memory Checkpoints, a hard limit is only imposed for Disk
     * Checkpoints (i.e. replica backfills).
     * Atomic as can be changed by ValueChangedListener on one thread and read
     * by flusher on other thread.
     */
    std::atomic<size_t> flusherBatchSplitTrigger;

    /**
     * Indicates whether erroneous tombstones need to retained or not during
     * compaction
     */
    cb::RelaxedAtomic<bool> retainErroneousTombstones;

    std::unique_ptr<Warmup> warmupTask;

    std::vector<std::unique_ptr<BgFetcher>> bgFetchers;

    folly::Synchronized<std::unordered_map<Vbid, std::shared_ptr<CompactTask>>>
            compactionTasks;

    /**
     * Testing hook called after we updated stats in the compactionCompletion
     * function
     */
    TestingHook<> postCompactionCompletionStatsUpdateHook;
};

std::ostream& operator<<(std::ostream& os, const EPBucket::FlushResult& res);

/**
 * Callback for notifying flusher about pending mutations.
 */
class NotifyFlusherCB : public Callback<Vbid> {
public:
    NotifyFlusherCB(KVShard* sh) : shard(sh) {
    }

    void callback(Vbid& vb) override;

private:
    KVShard* shard;
};
