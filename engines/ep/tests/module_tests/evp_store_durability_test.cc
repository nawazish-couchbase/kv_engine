/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc
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

#include "evp_store_durability_test.h"
#include "../mock/mock_synchronous_ep_engine.h"
#include "checkpoint_utils.h"
#include "durability/durability_monitor.h"
#include "test_helpers.h"

#include <engines/ep/src/ephemeral_tombstone_purger.h>
#include <engines/ep/tests/mock/mock_paging_visitor.h>
#include <programs/engine_testapp/mock_server.h>

class DurabilityEPBucketTest : public STParameterizedBucketTest {
protected:
    void SetUp() {
        STParameterizedBucketTest::SetUp();
        // Add an initial replication topology so we can accept SyncWrites.
        setVBucketToActiveWithValidTopology();
    }

    void setVBucketToActiveWithValidTopology(
            nlohmann::json topology = nlohmann::json::array({{"active",
                                                              "replica"}})) {
        setVBucketStateAndRunPersistTask(
                vbid, vbucket_state_active, {{"topology", topology}});
    }

    /// Test that a prepare of a SyncWrite / SyncDelete is correctly persisted
    /// to disk.
    void testPersistPrepare(DocumentState docState);

    /// Test that a prepare of a SyncWrite / SyncDelete, which is then aborted
    /// is correctly persisted to disk.
    void testPersistPrepareAbort(DocumentState docState);

    /**
     * Test that if a single key is prepared, aborted & re-prepared it is the
     * second Prepare which is kept on disk.
     * @param docState State to use for the second prepare (first is always
     *                 state alive).
     */
    void testPersistPrepareAbortPrepare(DocumentState docState);

    /**
     * Test that if a single key is prepared, aborted re-prepared & re-aborted
     * it is the second Abort which is kept on disk.
     * @param docState State to use for the second prepare (first is always
     *                 state alive).
     */
    void testPersistPrepareAbortX2(DocumentState docState);

    /**
     * Method to verify that a document of a given key is delete
     * @param vb the vbucket that should contain the deleted document
     * @param key the key of the document that should have been delete
     */
    void verifyDocumentIsDelete(VBucket& vb, StoredDocKey key);

    /**
     * Method to verify that a document is present in a vbucket
     * @param vb the vbucket that should contain the stored document
     * @param key the key of the stored document
     */
    void verifyDocumentIsStored(VBucket& vb, StoredDocKey key);

    /**
     * Method to create a SyncWrite by calling store, check the on disk
     * item count and collection count after calling the store
     * @param vb vbucket to perform the prepare SyncWrite to
     * @param pendingItem the new mutation that should be written to disk
     * @param expectedDiskCount expected number of items on disk after the
     * call to store
     * @param expectedCollectedCount expected number of items in the default
     * collection after the call to store
     */
    void performPrepareSyncWrite(VBucket& vb,
                                 queued_item pendingItem,
                                 uint64_t expectedDiskCount,
                                 uint64_t expectedCollectedCount);

    /**
     * Method to create a SyncDelete by calling delete on the vbucket, check
     * the on disk item count and collection count after calling the store
     * @param vb vbucket to perform the prepare SyncDelete to
     * @param key of the document to be deleted
     * @param expectedDiskCount expected number of items on disk after the
     * call to delete
     * @param expectedCollectedCount expected number of items in the default
     * collection after the call to delete
     */
    void performPrepareSyncDelete(VBucket& vb,
                                  StoredDocKey key,
                                  uint64_t expectedDiskCount,
                                  uint64_t expectedCollectedCount);

    /**
     * Method to perform a commit of a mutation for a given key and
     * check the on disk item count afterwards.
     * @param vb vbucket to perform the commit to
     * @param key StoredDockKey that we should be committing
     * @param prepareSeqno the prepare seqno of the commit
     * @param expectedDiskCount expected number of items on disk after the
     * commit
     * @param expectedCollectedCount expected number of items in the default
     * collection after the commit
     */
    void performCommitForKey(VBucket& vb,
                             StoredDocKey key,
                             uint64_t prepareSeqno,
                             uint64_t expectedDiskCount,
                             uint64_t expectedCollectedCount);

    /**
     * Method to perform an end to end SyncWrite by creating an document of
     * keyName with the value value and then performing a flush of the
     * prepare and committed mutations.
     * @param vb vbucket to perform the SyncWrite on
     * @param keyName name of the key to perform a SyncWrite too
     * @param value that should be written to the key
     */
    void testCommittedSyncWriteFlushAfterCommit(VBucket& vb,
                                                std::string keyName,
                                                std::string value);

    /**
     * Method to perform an end to end SyncDelete for the document with the key
     * keyName. We also flush the prepare and commit of the SyncDelete to disk
     * and then check that the value has been written to disk.
     * prepare and committed mutations.
     * @param vb vbucket to perform the SyncDelete on
     * @param keyName the name of the key to delete
     */
    void testSyncDeleteFlushAfterCommit(VBucket& vb, std::string keyName);

    /**
     * Method to verify a vbucket's on disk item count
     * @param vb VBucket reference that stores the on disk count that we want
     * to assert
     * @param expectedValue The value of the on disk item count that expect the
     * counter to be.
     */
    void verifyOnDiskItemCount(VBucket& vb, uint64_t expectedValue);

    /**
     * Method to verify collection's item count
     * @param vb VBucket reference that stores the on disk count that we want
     * to assert
     * @param cID The CollectionID of the collection that contains the on disk
     * count that we want to assert against the expectedValue
     * @param expectedValue The value of the on disk item count that expect the
     * counter to be.
     */
    void verifyCollectionItemCount(VBucket& vb,
                                   CollectionID cID,
                                   uint64_t expectedValue);
};

/**
 * Test fixture for Durability-related tests applicable to ephemeral and
 * persistent buckets with either eviction modes.
 */
class DurabilityBucketTest : public STParameterizedBucketTest {
protected:
    void setVBucketToActiveWithValidTopology(
            nlohmann::json topology = nlohmann::json::array({{"active",
                                                              "replica"}})) {
        setVBucketStateAndRunPersistTask(
                vbid, vbucket_state_active, {{"topology", topology}});
    }

    template <typename F>
    void testDurabilityInvalidLevel(F& func);

    /**
     * MB-34770: Test that a Pending -> Active takeover (which has in-flight
     * prepared SyncWrites) is handled correctly when there isn't yet a
     * replication toplogy.
     * This is the case during takeover where the setvbstate(active) is sent
     * from the old active which doesn't know what the topology will be and
     * hence is null.
     */
    void testTakeoverDestinationHandlesPreparedSyncWrites(
            cb::durability::Level level);
};

class DurabilityEphemeralBucketTest : public STParameterizedBucketTest {
protected:
    template <typename F>
    void testPurgeCompletedPrepare(F& func);
};

/// Note - not single-threaded
class DurabilityRespondAmbiguousTest : public KVBucketTest {
protected:
    void SetUp() override {
        // The test should do the SetUp
    }
    void TearDown() override {
            // The test should do the TearDown
    };
};

void DurabilityEPBucketTest::testPersistPrepare(DocumentState docState) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto key = makeStoredDocKey("key");
    auto committed = makeCommittedItem(key, "valueA");
    ASSERT_EQ(ENGINE_SUCCESS, store->set(*committed, cookie));
    auto& vb = *store->getVBucket(vbid);
    flushVBucketToDiskIfPersistent(vbid, 1);
    ASSERT_EQ(1, vb.getNumItems());
    auto pending = makePendingItem(key, "valueB");
    if (docState == DocumentState::Deleted) {
        pending->setDeleted(DeleteSource::Explicit);
    }
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    ASSERT_EQ(1, ckptMgr.getNumItemsForPersistence());
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    // Committed and Pending will be split in one checkpoint
    ASSERT_EQ(1, ckptList.size());

    const auto& stats = engine->getEpStats();
    ASSERT_EQ(1, stats.diskQueueSize);

    // Item must be flushed
    flushVBucketToDiskIfPersistent(vbid, 1);

    // Item must have been removed from the disk queue
    EXPECT_EQ(0, ckptMgr.getNumItemsForPersistence());
    EXPECT_EQ(0, stats.diskQueueSize);

    // The item count must not increase when flushing Pending SyncWrites
    EXPECT_EQ(1, vb.getNumItems());

    // @TODO RocksDB
    // @TODO Durability
    // TSan sporadically reports a data race when calling store->get below when
    // running this test under RocksDB. Manifests for both full and value
    // eviction but only seen after adding full eviction variants for this test.
    // Might be the case that running the couchstore full eviction variant
    // beforehand is breaking something.
#ifdef THREAD_SANITIZER
    auto bucketType = std::get<0>(GetParam());
    if (bucketType == "persistentRocksdb") {
        return;
    }
#endif

    // Check the committed item on disk.
    auto* store = vb.getShard()->getROUnderlying();
    auto gv = store->get(DiskDocKey(key), Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_EQ(*committed, *gv.item);

    // Check the Prepare on disk
    DiskDocKey prefixedKey(key, true /*prepare*/);
    gv = store->get(prefixedKey, Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isPending());
    EXPECT_EQ(docState == DocumentState::Deleted, gv.item->isDeleted());
}

TEST_P(DurabilityEPBucketTest, PersistPrepareWrite) {
    testPersistPrepare(DocumentState::Alive);
}

TEST_P(DurabilityEPBucketTest, PersistPrepareDelete) {
    testPersistPrepare(DocumentState::Deleted);
}

void DurabilityEPBucketTest::testPersistPrepareAbort(DocumentState docState) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);
    ASSERT_EQ(0, vb.getNumItems());

    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    if (docState == DocumentState::Deleted) {
        pending->setDeleted(DeleteSource::Explicit);
    }
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    // A Prepare doesn't account in curr-items
    ASSERT_EQ(0, vb.getNumItems());

    {
        auto res = vb.ht.findForWrite(key);
        ASSERT_TRUE(res.storedValue);
        ASSERT_EQ(CommittedState::Pending, res.storedValue->getCommitted());
        ASSERT_EQ(1, res.storedValue->getBySeqno());
    }
    const auto& stats = engine->getEpStats();
    ASSERT_EQ(1, stats.diskQueueSize);
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    ASSERT_EQ(1, ckptMgr.getNumItemsForPersistence());
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(1, ckptList.size());
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckptList.front()->getState());
    ASSERT_EQ(1, ckptList.front()->getNumItems());
    ASSERT_EQ(1,
              (*(--ckptList.front()->end()))->getOperation() ==
                      queue_op::pending_sync_write);

    ASSERT_EQ(ENGINE_SUCCESS,
              vb.abort(key,
                       1 /*prepareSeqno*/,
                       {} /*abortSeqno*/,
                       vb.lockCollections(key)));

    // We do not deduplicate Prepare and Abort (achieved by inserting them into
    // 2 different checkpoints)
    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(checkpoint_state::CHECKPOINT_OPEN, ckptList.back()->getState());
    ASSERT_EQ(1, ckptList.back()->getNumItems());
    ASSERT_EQ(1,
              (*(--ckptList.back()->end()))->getOperation() ==
                      queue_op::abort_sync_write);
    EXPECT_EQ(2, ckptMgr.getNumItemsForPersistence());
    EXPECT_EQ(2, stats.diskQueueSize);

    // Note: Prepare and Abort are in the same key-space, so they will be
    //     deduplicated at Flush
    flushVBucketToDiskIfPersistent(vbid, 1);

    EXPECT_EQ(0, vb.getNumItems());
    EXPECT_EQ(0, ckptMgr.getNumItemsForPersistence());
    EXPECT_EQ(0, stats.diskQueueSize);

    // At persist-dedup, the Abort survives
    auto* store = vb.getShard()->getROUnderlying();
    DiskDocKey prefixedKey(key, true /*pending*/);
    auto gv = store->get(prefixedKey, Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isAbort());
    EXPECT_TRUE(gv.item->isDeleted());
    EXPECT_NE(0, gv.item->getDeleteTime());
}

TEST_P(DurabilityEPBucketTest, PersistPrepareWriteAbort) {
    testPersistPrepareAbort(DocumentState::Alive);
}

TEST_P(DurabilityEPBucketTest, PersistPrepareDeleteAbort) {
    testPersistPrepareAbort(DocumentState::Deleted);
}

void DurabilityEPBucketTest::testPersistPrepareAbortPrepare(
        DocumentState docState) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    // First prepare (always a SyncWrite) and abort.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.abort(key,
                       pending->getBySeqno(),
                       {} /*abortSeqno*/,
                       vb.lockCollections(key)));

    // Second prepare.
    auto pending2 = makePendingItem(key, "value2");
    if (docState == DocumentState::Deleted) {
        pending2->setDeleted(DeleteSource::Explicit);
    }
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending2, cookie));

    // We do not deduplicate Prepare and Abort (achieved by inserting them into
    // different checkpoints)
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(3, ckptList.size());
    ASSERT_EQ(1, ckptList.back()->getNumItems());
    ASSERT_EQ(1,
              (*(--ckptList.back()->end()))->getOperation() ==
                      queue_op::pending_sync_write);
    EXPECT_EQ(3, ckptMgr.getNumItemsForPersistence());

    // Note: Prepare and Abort are in the same key-space, so they will be
    //     deduplicated at Flush
    flushVBucketToDiskIfPersistent(vbid, 1);

    // At persist-dedup, the 2nd Prepare survives
    auto* store = vb.getShard()->getROUnderlying();
    DiskDocKey prefixedKey(key, true /*pending*/);
    auto gv = store->get(prefixedKey, Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isPending());
    EXPECT_EQ(docState == DocumentState::Deleted, gv.item->isDeleted());
    EXPECT_EQ(pending2->getBySeqno(), gv.item->getBySeqno());
}

TEST_P(DurabilityEPBucketTest, PersistPrepareAbortPrepare) {
    testPersistPrepareAbortPrepare(DocumentState::Alive);
}

TEST_P(DurabilityEPBucketTest, PersistPrepareAbortPrepareDelete) {
    testPersistPrepareAbortPrepare(DocumentState::Deleted);
}

void DurabilityEPBucketTest::testPersistPrepareAbortX2(DocumentState docState) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    // First prepare and abort.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.abort(key,
                       pending->getBySeqno(),
                       {} /*abortSeqno*/,
                       vb.lockCollections(key)));

    // Second prepare and abort.
    auto pending2 = makePendingItem(key, "value2");
    if (docState == DocumentState::Deleted) {
        pending2->setDeleted(DeleteSource::Explicit);
    }
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending2, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.abort(key,
                       pending2->getBySeqno(),
                       {} /*abortSeqno*/,
                       vb.lockCollections(key)));

    // We do not deduplicate Prepare and Abort (achieved by inserting them into
    // different checkpoints)
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(4, ckptList.size());
    ASSERT_EQ(1, ckptList.back()->getNumItems());
    ASSERT_EQ(1,
              (*(--ckptList.back()->end()))->getOperation() ==
                      queue_op::abort_sync_write);
    EXPECT_EQ(4, ckptMgr.getNumItemsForPersistence());

    // Note: Prepare and Abort are in the same key-space and hence are
    //       deduplicated at Flush.
    flushVBucketToDiskIfPersistent(vbid, 1);

    // At persist-dedup, the 2nd Abort survives
    auto* store = vb.getShard()->getROUnderlying();
    DiskDocKey prefixedKey(key, true /*pending*/);
    auto gv = store->get(prefixedKey, Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isAbort());
    EXPECT_TRUE(gv.item->isDeleted());
    EXPECT_EQ(pending2->getBySeqno() + 1, gv.item->getBySeqno());
}

TEST_P(DurabilityEPBucketTest, PersistPrepareAbortx2) {
    testPersistPrepareAbortX2(DocumentState::Alive);
}

TEST_P(DurabilityEPBucketTest, PersistPrepareAbortPrepareDeleteAbort) {
    testPersistPrepareAbortX2(DocumentState::Deleted);
}

/// Test persistence of a prepared & committed SyncWrite, followed by a
/// prepared & committed SyncDelete.
TEST_P(DurabilityEPBucketTest, PersistSyncWriteSyncDelete) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    // prepare SyncWrite and commit.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        pending->getBySeqno(),
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    // We do not deduplicate Prepare and Commit in CheckpointManager but they
    // can exist in a single checkpoint
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(1, ckptList.size());
    ASSERT_EQ(2, ckptList.back()->getNumItems());
    EXPECT_EQ(2, ckptMgr.getNumItemsForPersistence());

    // Note: Prepare and Commit are not in the same key-space and hence are not
    //       deduplicated at Flush.
    flushVBucketToDiskIfPersistent(vbid, 2);

    // prepare SyncDelete and commit.
    uint64_t cas = 0;
    using namespace cb::durability;
    auto reqs = Requirements(Level::Majority, {});
    mutation_descr_t delInfo;
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->deleteItem(key, cas, vbid, cookie, reqs, nullptr, delInfo));

    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(1, ckptList.back()->getNumItems());
    EXPECT_EQ(1, ckptMgr.getNumItemsForPersistence());

    flushVBucketToDiskIfPersistent(vbid, 1);

    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        delInfo.seqno,
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(2, ckptList.back()->getNumItems());
    EXPECT_EQ(1, ckptMgr.getNumItemsForPersistence());

    flushVBucketToDiskIfPersistent(vbid, 1);

    // At persist-dedup, the 2nd Prepare and Commit survive.
    auto* store = vb.getShard()->getROUnderlying();
    auto gv = store->get(DiskDocKey(key), Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isCommitted());
    EXPECT_TRUE(gv.item->isDeleted());
    EXPECT_EQ(delInfo.seqno + 1, gv.item->getBySeqno());
}

/// Test SyncDelete on top of SyncWrite
TEST_P(DurabilityBucketTest, SyncWriteSyncDelete) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    // prepare SyncWrite and commit.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        pending->getBySeqno(),
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    // We do not deduplicate Prepare and Commit in CheckpointManager (achieved
    // by inserting them into different checkpoints)
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(1, ckptList.size());
    ASSERT_EQ(2, ckptList.back()->getNumItems());

    // Note: Prepare and Commit are not in the same key-space and hence are not
    //       deduplicated at Flush.
    flushVBucketToDiskIfPersistent(vbid, 2);

    // prepare SyncDelete and commit.
    uint64_t cas = 0;
    using namespace cb::durability;
    auto reqs = Requirements(Level::Majority, {});
    mutation_descr_t delInfo;

    EXPECT_EQ(1, vb.getNumItems());

    // Ephemeral keeps the completed prepare
    if (persistent()) {
        EXPECT_EQ(0, vb.ht.getNumPreparedSyncWrites());
    } else {
        EXPECT_EQ(1, vb.ht.getNumPreparedSyncWrites());
    }
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->deleteItem(key, cas, vbid, cookie, reqs, nullptr, delInfo));

    EXPECT_EQ(1, vb.getNumItems());
    EXPECT_EQ(1, vb.ht.getNumPreparedSyncWrites());

    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(1, ckptList.back()->getNumItems());

    flushVBucketToDiskIfPersistent(vbid, 1);

    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        3 /*prepareSeqno*/,
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    flushVBucketToDiskIfPersistent(vbid, 1);

    EXPECT_EQ(0, vb.getNumItems());

    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(2, ckptList.back()->getNumItems());
}

// Test SyncDelete followed by a SyncWrite, where persistence of
// SyncDelete's Commit is delayed until SyncWrite prepare in HashTable (checking
// correct HashTable item is removed)
// Regression test for MB-34810.
TEST_P(DurabilityBucketTest, SyncDeleteSyncWriteDelayedPersistence) {
    // Setup: Add an initial value (so we can SyncDelete it).
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);
    auto key = makeStoredDocKey("key");
    auto committed = makeCommittedItem(key, "valueA");
    ASSERT_EQ(ENGINE_SUCCESS, store->set(*committed, cookie));

    // Setup: prepare SyncDelete
    uint64_t cas = 0;
    using namespace cb::durability;
    auto reqs = Requirements(Level::Majority, {});
    mutation_descr_t delInfo;
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->deleteItem(key, cas, vbid, cookie, reqs, nullptr, delInfo));

    // Setup: Persist SyncDelete prepare.
    flushVBucketToDiskIfPersistent(vbid, 2);

    // Setup: commit SyncDelete (but no flush yet).
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        3 /*prepareSeqno*/,
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    // Setuo: Prepare SyncWrite
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

    // Test: flush items to disk. The flush of the Committed SyncDelete will
    // attempt to remove that item from the HashTable; check the correct item
    // is removed (Committed SyncDelete, not prepared SyncWrite).
    flushVBucketToDiskIfPersistent(vbid, 2);

    EXPECT_EQ(1, vb.ht.getNumPreparedSyncWrites())
            << "SyncWrite prepare should still exist";

    EXPECT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        5 /*prepareSeqno*/,
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)))
            << "SyncWrite commit should be possible";
}

/// Test delete on top of SyncWrite
TEST_P(DurabilityBucketTest, SyncWriteDelete) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    // prepare SyncWrite and commit.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        pending->getBySeqno(),
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));

    // We do not deduplicate Prepare and Commit in CheckpointManager (achieved
    // by inserting them into different checkpoints)
    const auto& ckptMgr = *store->getVBucket(vbid)->checkpointManager;
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    ckptMgr);
    ASSERT_EQ(1, ckptList.size());
    ASSERT_EQ(2, ckptList.back()->getNumItems());

    // Note: Prepare and Commit are not in the same key-space and hence are not
    //       deduplicated at Flush.
    flushVBucketToDiskIfPersistent(vbid, 2);

    // Perform regular delete.
    uint64_t cas = 0;
    mutation_descr_t delInfo;

    EXPECT_EQ(1, vb.getNumItems());

    auto expectedNumPrepares = persistent() ? 0 : 1;
    EXPECT_EQ(expectedNumPrepares, vb.ht.getNumPreparedSyncWrites());
    ASSERT_EQ(ENGINE_SUCCESS,
              store->deleteItem(key, cas, vbid, cookie, {}, nullptr, delInfo));

    flushVBucketToDiskIfPersistent(vbid, 1);

    EXPECT_EQ(0, vb.getNumItems());
    EXPECT_EQ(expectedNumPrepares, vb.ht.getNumPreparedSyncWrites());

    ASSERT_EQ(2, ckptList.size());
    ASSERT_EQ(1, ckptList.back()->getNumItems());
}

void DurabilityEPBucketTest::verifyOnDiskItemCount(VBucket& vb,
                                                   uint64_t expectedValue) {
    // skip for rocksdb as it treats every mutation as an insertion
    // and so we would expect a different item count compared with couchstore
    auto bucketType = std::get<0>(GetParam());
    if (bucketType == "persistentRocksdb") {
        return;
    }
    EXPECT_EQ(expectedValue, vb.getNumTotalItems());
}

void DurabilityEPBucketTest::verifyCollectionItemCount(VBucket& vb,
                                                       CollectionID cID,
                                                       uint64_t expectedValue) {
    // skip for rocksdb as it dose not perform item counting for collections
    auto bucketType = std::get<0>(GetParam());
    if (bucketType == "persistentRocksdb") {
        return;
    }
    {
        auto rh = vb.lockCollections();
        EXPECT_EQ(expectedValue, rh.getItemCount(cID));
    }
}

void DurabilityEPBucketTest::verifyDocumentIsStored(VBucket& vb,
                                                    StoredDocKey key) {
    auto* store = vb.getShard()->getROUnderlying();
    auto gv = store->get(DiskDocKey(key), Vbid(0));
    ASSERT_EQ(ENGINE_SUCCESS, gv.getStatus());
    ASSERT_FALSE(gv.item->isDeleted());
    ASSERT_TRUE(gv.item->isCommitted());
}

void DurabilityEPBucketTest::verifyDocumentIsDelete(VBucket& vb,
                                                    StoredDocKey key) {
    auto* store = vb.getShard()->getROUnderlying();
    auto gv = store->get(DiskDocKey(key), Vbid(0));
    ASSERT_EQ(ENGINE_SUCCESS, gv.getStatus());
    ASSERT_TRUE(gv.item->isDeleted());
    ASSERT_TRUE(gv.item->isCommitted());
}

void DurabilityEPBucketTest::performPrepareSyncWrite(
        VBucket& vb,
        queued_item pendingItem,
        uint64_t expectedDiskCount,
        uint64_t expectedCollectedCount) {
    auto cID = pendingItem->getKey().getCollectionID();
    // First prepare SyncWrite and commit for test_doc.
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pendingItem, cookie));
    verifyOnDiskItemCount(vb, expectedDiskCount);
    verifyCollectionItemCount(vb, cID, expectedCollectedCount);
}

void DurabilityEPBucketTest::performPrepareSyncDelete(
        VBucket& vb,
        StoredDocKey key,
        uint64_t expectedDiskCount,
        uint64_t expectedCollectedCount) {
    mutation_descr_t delInfo;
    uint64_t cas = 0;
    auto reqs =
            cb::durability::Requirements(cb::durability::Level::Majority, {});
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->deleteItem(key, cas, vbid, cookie, reqs, nullptr, delInfo));

    verifyOnDiskItemCount(vb, expectedDiskCount);
    verifyCollectionItemCount(
            vb, key.getCollectionID(), expectedCollectedCount);
}

void DurabilityEPBucketTest::performCommitForKey(
        VBucket& vb,
        StoredDocKey key,
        uint64_t prepareSeqno,
        uint64_t expectedDiskCount,
        uint64_t expectedCollectedCount) {
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.commit(key,
                        prepareSeqno,
                        {} /*commitSeqno*/,
                        vb.lockCollections(key)));
    verifyOnDiskItemCount(vb, expectedDiskCount);
    verifyCollectionItemCount(
            vb, key.getCollectionID(), expectedCollectedCount);
}

void DurabilityEPBucketTest::testCommittedSyncWriteFlushAfterCommit(
        VBucket& vb, std::string keyName, std::string value) {
    // prepare SyncWrite and commit.
    auto key = makeStoredDocKey(keyName);
    auto keyCollectionID = key.getCollectionID();
    auto pending = makePendingItem(key, value);

    auto initOnDiskCount = vb.getNumTotalItems();
    uint64_t currentCollectionCount = 0;
    {
        auto rh = vb.lockCollections();
        currentCollectionCount = rh.getItemCount(keyCollectionID);
    }

    performPrepareSyncWrite(
            vb, pending, initOnDiskCount, currentCollectionCount);
    auto prepareSeqno = vb.getHighSeqno();
    performCommitForKey(
            vb, key, prepareSeqno, initOnDiskCount, currentCollectionCount);

    // Note: Prepare and Commit are not in the same key-space and hence are not
    //       deduplicated at Flush.
    flushVBucketToDiskIfPersistent(vbid, 2);

    // check the value is correctly set on disk
    verifyDocumentIsStored(vb, key);
}

void DurabilityEPBucketTest::testSyncDeleteFlushAfterCommit(
        VBucket& vb, std::string keyName) {
    auto key = makeStoredDocKey(keyName);
    auto keyCollectionID = key.getCollectionID();

    auto initOnDiskCount = vb.getNumTotalItems();
    uint64_t currentCollectionCount = 0;
    {
        auto rh = vb.lockCollections();
        currentCollectionCount = rh.getItemCount(keyCollectionID);
    }

    performPrepareSyncDelete(vb, key, initOnDiskCount, currentCollectionCount);
    auto prepareSeqno = vb.getHighSeqno();
    performCommitForKey(
            vb, key, prepareSeqno, initOnDiskCount, currentCollectionCount);

    // flush the prepare and commit mutations to disk
    flushVBucketToDiskIfPersistent(vbid, 2);

    // check the value is correctly deleted on disk
    verifyDocumentIsDelete(vb, key);
}

/// Test persistence of a prepared & committed SyncWrite, a second prepared
/// & committed SyncWrite, followed by a prepared & committed SyncDelete.
TEST_P(DurabilityEPBucketTest, PersistSyncWriteSyncWriteSyncDelete) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, 0, 0);

    // prepare SyncWrite and commit.
    testCommittedSyncWriteFlushAfterCommit(vb, "key", "value");
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, 0, 1);

    // Second prepare SyncWrite and commit.
    testCommittedSyncWriteFlushAfterCommit(vb, "key", "value2");
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, 0, 1);

    // prepare SyncDelete and commit.
    auto key = makeStoredDocKey("key");
    performPrepareSyncDelete(vb, key, 1, 1);
    auto prepareSeqno = vb.getHighSeqno();

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, key.getCollectionID(), 1);

    performCommitForKey(vb, key, prepareSeqno, 1, 1);

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, key.getCollectionID(), 0);

    // At persist-dedup, the 2nd Prepare and Commit survive.
    verifyDocumentIsDelete(vb, key);
}

/**
 * Test to check that our on disk count and collections count are tracked
 * correctly and do not underflow.
 *
 * This test does two rounds of SyncWrite then SyncDelete of a document with
 * the same key called "test_doc". Before the fix for MB-34094 and MB-34120 we
 * would expect our on disk counters to underflow and throw and exception.
 *
 * Note in this version of the test we flush after each commit made.
 */
TEST_P(DurabilityEPBucketTest,
       PersistSyncWriteSyncDeleteTwiceFlushAfterEachCommit) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    auto& vb = *store->getVBucket(vbid);

    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, 0, 0);

    // First prepare SyncWrite and commit for test_doc.
    testCommittedSyncWriteFlushAfterCommit(vb, "test_doc", "{ \"run\": 1 }");
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, 0, 1);

    // First prepare SyncDelete and commit.
    testSyncDeleteFlushAfterCommit(vb, "test_doc");
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, 0, 0);

    // Second prepare SyncWrite and commit.
    testCommittedSyncWriteFlushAfterCommit(vb, "test_doc", "{ \"run\": 2 }");
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, 0, 1);

    // Second prepare SyncDelete and commit.
    testSyncDeleteFlushAfterCommit(vb, "test_doc");
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, 0, 0);
}

/**
 * Test to check that our on disk count and collections count are track
 * correctly and do not underflow.
 *
 * This test does two rounds of SyncWrite then SyncDelete of a document with
 * the same key called "test_doc". Before the fix for MB-34094 and MB-34120 we
 * would expect our on disk counters to underflow and throw and exception.
 *
 * Note in this version of the test we flush after all commits an prepares
 * have been made.
 */
TEST_P(DurabilityEPBucketTest,
       PersistSyncWriteSyncDeleteTwiceFlushAfterAllMutations) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    using namespace cb::durability;
    auto& vb = *store->getVBucket(vbid);
    auto* kvstore = vb.getShard()->getROUnderlying();

    std::string keyName("test_doc");
    auto key = makeStoredDocKey(keyName);
    auto keyCollectionID = key.getCollectionID();
    auto pending = makePendingItem(key, "{ \"run\": 1 }");

    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    // First prepare SyncWrite and commit for test_doc.
    performPrepareSyncWrite(vb, pending, 0, 0);
    auto prepareSeqno = vb.getHighSeqno();
    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    // check the value is correctly set on disk
    auto gv = kvstore->get(DiskDocKey(key), Vbid(0));
    ASSERT_EQ(ENGINE_KEY_ENOENT, gv.getStatus());

    // First prepare SyncDelete and commit.
    performPrepareSyncDelete(vb, key, 0, 0);
    prepareSeqno = vb.getHighSeqno();
    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    // check the value is correctly deleted on disk
    gv = kvstore->get(DiskDocKey(key), Vbid(0));
    ASSERT_EQ(ENGINE_KEY_ENOENT, gv.getStatus());

    // Second prepare SyncWrite and commit.
    pending = makePendingItem(key, "{ \"run\": 2 }");
    performPrepareSyncWrite(vb, pending, 0, 0);
    prepareSeqno = vb.getHighSeqno();
    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    // check the value is correctly set on disk
    gv = kvstore->get(DiskDocKey(key), Vbid(0));
    ASSERT_EQ(ENGINE_KEY_ENOENT, gv.getStatus());

    // Second prepare SyncDelete and commit.
    performPrepareSyncDelete(vb, key, 0, 0);
    prepareSeqno = vb.getHighSeqno();
    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    // flush the prepare and commit mutations to disk
    flushVBucketToDiskIfPersistent(vbid, 2);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    // check the value is correctly deleted on disk
    verifyDocumentIsDelete(vb, key);
}

/**
 * Test to check that our on disk count and collections count are track
 * correctly and do not underflow.
 *
 * This test does two rounds of SyncWrite then SyncDelete of a document with
 * the same key called "test_doc". Before the fix for MB-34094 and MB-34120 we
 * would expect our on disk counters to underflow and throw and exception.
 *
 * Note in this version of the test we flush after each commit and prepare made.
 */
TEST_P(DurabilityEPBucketTest,
       PersistSyncWriteSyncDeleteTwiceFlushAfterEachMutation) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    using namespace cb::durability;
    auto& vb = *store->getVBucket(vbid);

    std::string keyName("test_doc");
    auto key = makeStoredDocKey(keyName);
    auto keyCollectionID = key.getCollectionID();
    auto pending = makePendingItem(key, "{ \"run\": 1 }");

    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    // First prepare SyncWrite and commit for test_doc.
    performPrepareSyncWrite(vb, pending, 0, 0);
    auto prepareSeqno = vb.getHighSeqno();

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, keyCollectionID, 1);

    // check the value is correctly set on disk
    verifyDocumentIsStored(vb, key);

    // First prepare SyncDelete and commit.
    performPrepareSyncDelete(vb, key, 1, 1);
    prepareSeqno = vb.getHighSeqno();

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, keyCollectionID, 1);

    performCommitForKey(vb, key, prepareSeqno, 1, 1);

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    // check the value is correctly deleted on disk
    verifyDocumentIsDelete(vb, key);

    // Second prepare SyncWrite and commit.
    pending = makePendingItem(key, "{ \"run\": 2 }");
    performPrepareSyncWrite(vb, pending, 0, 0);
    prepareSeqno = vb.getHighSeqno();

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    performCommitForKey(vb, key, prepareSeqno, 0, 0);

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, keyCollectionID, 1);

    // check the value is correctly set on disk
    verifyDocumentIsStored(vb, key);

    // Second prepare SyncDelete and commit.
    performPrepareSyncDelete(vb, key, 1, 1);
    prepareSeqno = vb.getHighSeqno();

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 1);
    verifyCollectionItemCount(vb, keyCollectionID, 1);

    performCommitForKey(vb, key, prepareSeqno, 1, 1);

    flushVBucketToDiskIfPersistent(vbid, 1);
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, keyCollectionID, 0);

    // check the value is correctly deleted on disk
    verifyDocumentIsDelete(vb, key);
}

/**
 * Test to check that our on disk count and collections count are track
 * correctly and do not underflow.
 *
 * This test does 3 rounds of SyncWrite then SyncDelete of a document with
 * for a set of documents "test_doc-{0..9}". Before the fix for MB-34094 and
 * MB-34120 we would expect our on disk counters to underflow and throw and
 * exception.
 *
 * This performs multiple runs with ten documents as this allows us to perform
 * a sanity test that when we are setting and deleting more than one document
 * that our on disk accounting remain consistent.
 */
TEST_P(DurabilityEPBucketTest, PersistSyncWriteSyncDeleteTenDocs3Times) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    using namespace cb::durability;
    std::string keyName("test_doc-");
    auto& vb = *store->getVBucket(vbid);

    const uint32_t numberOfRuns = 3;
    const uint32_t numberOfDocks = 10;

    // Perform multiple runs of the creating and deletion of documents named
    // "test_doc-{0..9}".
    for (uint32_t j = 0; j < numberOfRuns; j++) {
        // Set and then delete ten documents names "test_doc-{0..9}"
        for (uint32_t i = 0; i < numberOfDocks; i++) {
            // prepare SyncWrite and commit.
            testCommittedSyncWriteFlushAfterCommit(
                    vb,
                    keyName + std::to_string(i),
                    "{ \"run\":" + std::to_string(j) + " }");
            verifyOnDiskItemCount(vb, 1);
            verifyCollectionItemCount(vb, 0, 1);

            // prepare SyncDelete and commit.
            testSyncDeleteFlushAfterCommit(vb, keyName + std::to_string(i));
            verifyOnDiskItemCount(vb, 0);
            verifyCollectionItemCount(vb, 0, 0);
        }
    }
}

/// Test to check that after 20 SyncWrites and then 20 SyncDeletes
/// that on disk count is 0.
/// Sanity test to make sure our accounting is consitant when we create
/// Multiple documents on disk.
TEST_P(DurabilityEPBucketTest, PersistSyncWrite20SyncDelete20) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    using namespace cb::durability;
    std::string keyName("test_doc-");
    auto& vb = *store->getVBucket(vbid);

    const uint32_t numberOfDocks = 20;
    // SyncWrite numberOfDocks docs
    for (uint32_t i = 0; i < numberOfDocks; i++) {
        // prepare SyncWrite and commit.
        testCommittedSyncWriteFlushAfterCommit(
                vb, keyName + std::to_string(i), "{ \"Hello\": \"World\" }");

        {
            SCOPED_TRACE("flush sync write: " + std::to_string(i));
            verifyOnDiskItemCount(vb, i + 1);
            verifyCollectionItemCount(vb, 0, i + 1);
        }
    }
    // SyncDelete Docs
    for (uint32_t i = 0; i < numberOfDocks; i++) {
        testSyncDeleteFlushAfterCommit(vb, keyName + std::to_string(i));
        {
            SCOPED_TRACE("flush sync delete: " + std::to_string(i));
            verifyOnDiskItemCount(vb, numberOfDocks - i - 1);
            verifyCollectionItemCount(vb, 0, numberOfDocks - i - 1);
        }
    }
    verifyOnDiskItemCount(vb, 0);
    verifyCollectionItemCount(vb, 0, 0);
}

TEST_P(DurabilityEPBucketTest, ActiveLocalNotifyPersistedSeqno) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});

    const cb::durability::Requirements reqs = {
            cb::durability::Level::PersistToMajority, {}};

    for (uint8_t seqno = 1; seqno <= 3; seqno++) {
        auto item = makePendingItem(
                makeStoredDocKey("key" + std::to_string(seqno)), "value", reqs);
        ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*item, cookie));
    }

    const auto& vb = store->getVBucket(vbid);
    const auto& ckptList =
            CheckpointManagerTestIntrospector::public_getCheckpointList(
                    *vb->checkpointManager);

    auto checkPending = [&ckptList]() -> void {
        ASSERT_EQ(1, ckptList.size());
        const auto& ckpt = *ckptList.front();
        EXPECT_EQ(3, ckpt.getNumItems());
        for (const auto& qi : ckpt) {
            if (!qi->isCheckPointMetaItem()) {
                EXPECT_EQ(queue_op::pending_sync_write, qi->getOperation());
            }
        }
    };

    // No replica has ack'ed yet
    checkPending();

    // Replica acks disk-seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              vb->seqnoAcknowledged(
                      folly::SharedMutex::ReadHolder(vb->getStateLock()),
                      "replica",
                      3 /*preparedSeqno*/));
    // Active has not persisted, so Durability Requirements not satisfied yet
    checkPending();

    // Flusher runs on Active. This:
    // - persists all pendings
    // - and notifies local DurabilityMonitor of persistence
    flushVBucketToDiskIfPersistent(vbid, 3);

    // When seqno:1 is persisted:
    //
    // - the Flusher notifies the local DurabilityMonitor
    // - seqno:1 is satisfied, so it is committed
    // - the next committed seqnos are enqueued into the same open checkpoint
    ASSERT_EQ(1, ckptList.size());
    const auto& ckpt = *ckptList.front();
    EXPECT_EQ(6, ckpt.getNumItems());
    for (const auto& qi : ckpt) {
        if (!qi->isCheckPointMetaItem()) {
            queue_op op;
            if (qi->getBySeqno() / 4 == 0) {
                // The first three non-meta items/seqnos are prepares
                op = queue_op::pending_sync_write;
            } else {
                // The rest (last 3) are commits
                op = queue_op::commit_sync_write;
            }
            EXPECT_EQ(op, qi->getOperation());
        }
    }
}

TEST_P(DurabilityEPBucketTest, SetDurabilityImpossible) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array({{"active", nullptr, nullptr}})}});

    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");

    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->set(*pending, cookie));

    auto item = makeCommittedItem(key, "value");
    EXPECT_NE(ENGINE_DURABILITY_IMPOSSIBLE, store->set(*item, cookie));
}

TEST_P(DurabilityEPBucketTest, AddDurabilityImpossible) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array({{"active", nullptr, nullptr}})}});

    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");

    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->add(*pending, cookie));

    auto item = makeCommittedItem(key, "value");
    EXPECT_NE(ENGINE_DURABILITY_IMPOSSIBLE, store->add(*item, cookie));
}

TEST_P(DurabilityEPBucketTest, ReplaceDurabilityImpossible) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array({{"active", nullptr, nullptr}})}});

    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");

    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->replace(*pending, cookie));

    auto item = makeCommittedItem(key, "value");
    EXPECT_NE(ENGINE_DURABILITY_IMPOSSIBLE, store->replace(*item, cookie));
}

TEST_P(DurabilityEPBucketTest, DeleteDurabilityImpossible) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array({{"active", nullptr, nullptr}})}});

    auto key = makeStoredDocKey("key");

    uint64_t cas = 0;
    mutation_descr_t mutation_descr;
    cb::durability::Requirements durabilityRequirements;
    durabilityRequirements.setLevel(cb::durability::Level::Majority);
    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE,
              store->deleteItem(key,
                                cas,
                                vbid,
                                cookie,
                                durabilityRequirements,
                                nullptr,
                                mutation_descr));

    durabilityRequirements.setLevel(cb::durability::Level::None);
    EXPECT_NE(ENGINE_DURABILITY_IMPOSSIBLE,
              store->deleteItem(key,
                                cas,
                                vbid,
                                cookie,
                                durabilityRequirements,
                                nullptr,
                                mutation_descr));
}

template <typename F>
void DurabilityBucketTest::testDurabilityInvalidLevel(F& func) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto reqs = Requirements(Level::Majority, {});
    auto pending = makePendingItem(key, "value", reqs);
    EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, func(pending, cookie));

    reqs = Requirements(Level::MajorityAndPersistOnMaster, {});
    pending = makePendingItem(key, "value", reqs);
    if (persistent()) {
        EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, func(pending, cookie));
    } else {
        EXPECT_EQ(ENGINE_DURABILITY_INVALID_LEVEL, func(pending, cookie));
    }

    reqs = Requirements(Level::PersistToMajority, {});
    pending = makePendingItem(key, "value", reqs);
    if (persistent()) {
        EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, func(pending, cookie));
    } else {
        EXPECT_EQ(ENGINE_DURABILITY_INVALID_LEVEL, func(pending, cookie));
    }
}

void DurabilityBucketTest::testTakeoverDestinationHandlesPreparedSyncWrites(
        cb::durability::Level level) {
    // Setup: VBucket into pending state with one Prepared SyncWrite.
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_pending);

    auto& vb = *store->getVBucket(vbid);
    vb.checkpointManager->createSnapshot(
            1, 1, {} /*HCS*/, CheckpointType::Memory);
    using namespace cb::durability;
    auto requirements = Requirements(level, Timeout::Infinity());
    auto pending =
            makePendingItem(makeStoredDocKey("key"), "value", requirements);
    pending->setCas(1);
    pending->setBySeqno(1);
    ASSERT_EQ(ENGINE_SUCCESS, store->prepare(*pending, nullptr));
    ASSERT_EQ(1, vb.getDurabilityMonitor().getNumTracked());

    // Test: Change to active via takeover (null topology),
    // then persist (including the prepared item above). This will trigger
    // the flusher to call back into ActiveDM telling it high prepared seqno
    // has advanced.
    EXPECT_EQ(ENGINE_SUCCESS,
              store->setVBucketState(
                      vbid, vbucket_state_active, {}, TransferVB::Yes));
    flushVBucketToDiskIfPersistent(vbid, 1);

    EXPECT_EQ(1, vb.getDurabilityMonitor().getNumTracked())
            << "Should have 1 prepared SyncWrite if active+null topology";

    // Test: Set the topology (as ns_server does), by specifying just
    // a single node in topology should now be able to commit the prepare.
    EXPECT_EQ(ENGINE_SUCCESS,
              store->setVBucketState(
                      vbid,
                      vbucket_state_active,
                      {{"topology", nlohmann::json::array({{"active"}})}}));
    // Given the prepare was already persisted to disk above when we first
    // changed to active, once a valid topology is set then SyncWrite should
    // be committed immediately irrespective of level.
    EXPECT_EQ(0, vb.getDurabilityMonitor().getNumTracked())
            << "Should have committed the SyncWrite if active+valid topology";
    // Should be able to flush Commit to disk.
    flushVBucketToDiskIfPersistent(vbid, 1);
}

TEST_P(DurabilityBucketTest, SetDurabilityInvalidLevel) {
    auto op = [this](queued_item pending,
                     const void* cookie) -> ENGINE_ERROR_CODE {
        return store->set(*pending, cookie);
    };
    testDurabilityInvalidLevel(op);
}

TEST_P(DurabilityBucketTest, AddDurabilityInvalidLevel) {
    auto op = [this](queued_item pending,
                     const void* cookie) -> ENGINE_ERROR_CODE {
        return store->add(*pending, cookie);
    };
    testDurabilityInvalidLevel(op);
}

TEST_P(DurabilityBucketTest, ReplaceDurabilityInvalidLevel) {
    auto op = [this](queued_item pending,
                     const void* cookie) -> ENGINE_ERROR_CODE {
        return store->replace(*pending, cookie);
    };
    testDurabilityInvalidLevel(op);
}

TEST_P(DurabilityBucketTest, DeleteDurabilityInvalidLevel) {
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_active);

    using namespace cb::durability;
    auto durabilityRequirements = Requirements(Level::Majority, {});

    auto del = [this](cb::durability::Requirements requirements)
            -> ENGINE_ERROR_CODE {
        auto key = makeStoredDocKey("key");
        uint64_t cas = 0;
        mutation_descr_t mutation_descr;
        return store->deleteItem(
                key, cas, vbid, cookie, requirements, nullptr, mutation_descr);
    };
    EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, del(durabilityRequirements));

    durabilityRequirements =
            Requirements(Level::MajorityAndPersistOnMaster, {});
    if (persistent()) {
        EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, del(durabilityRequirements));
    } else {
        EXPECT_EQ(ENGINE_DURABILITY_INVALID_LEVEL, del(durabilityRequirements));
    }

    durabilityRequirements = Requirements(Level::PersistToMajority, {});
    if (persistent()) {
        EXPECT_NE(ENGINE_DURABILITY_INVALID_LEVEL, del(durabilityRequirements));
    } else {
        EXPECT_EQ(ENGINE_DURABILITY_INVALID_LEVEL, del(durabilityRequirements));
    }
}

/// MB_34012: Test that add() returns DurabilityImpossible if there's already a
/// SyncWrite in progress against a key, instead of returning EEXISTS as add()
/// would normally if it found an existing item. (Until the first SyncWrite
/// completes there's no user-visible value for the key.
TEST_P(DurabilityBucketTest, AddIfAlreadyExistsSyncWriteInProgress) {
    setVBucketToActiveWithValidTopology();

    // Setup: Add the first prepared SyncWrite.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->add(*pending, cookie));

    // Test: Attempt to add a second prepared SyncWrite (different cookie i.e.
    // client).
    MockCookie secondClient;
    auto pending2 = makePendingItem(key, "value2");
    EXPECT_EQ(ENGINE_SYNC_WRITE_IN_PROGRESS,
              store->add(*pending2, &secondClient));
}

/// MB-35042: Test that SyncDelete returns SYNC_WRITE_IN_PROGRESS if there's
/// already a SyncDelete in progress against a key, instead of returning
/// KEY_ENOENT as delete() would normally if it didn't find an existing item.
TEST_P(DurabilityBucketTest, DeleteIfDeleteInProgressSyncWriteInProgress) {
    setVBucketToActiveWithValidTopology();

    // Setup: Create a document, then start a SyncDelete.
    auto key = makeStoredDocKey("key");
    auto committed = makeCommittedItem(key, "value");
    ASSERT_EQ(ENGINE_SUCCESS, store->set(*committed, cookie));
    uint64_t cas = 0;
    mutation_descr_t mutInfo;
    cb::durability::Requirements reqs{cb::durability::Level::Majority, {}};
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->deleteItem(key, cas, vbid, cookie, reqs, nullptr, mutInfo));

    // Test: Attempt to perform a second SyncDelete (different cookie i.e.
    // client).
    MockCookie secondClient;
    cas = 0;
    EXPECT_EQ(ENGINE_SYNC_WRITE_IN_PROGRESS,
              store->deleteItem(
                      key, cas, vbid, &secondClient, reqs, nullptr, mutInfo));
}

/// MB-35042: Test that SyncDelete returns SYNC_WRITE_IN_PROGRESS if there's
/// already a SyncWrite in progress against a key, instead of returning
/// KEY_ENOENT as delete() would normally if it found a deleted item in the
/// HashTable.
TEST_P(DurabilityBucketTest, DeleteIfSyncWriteInProgressSyncWriteInProgress) {
    setVBucketToActiveWithValidTopology();

    // Setup: start a SyncWrite.
    auto key = makeStoredDocKey("key");
    auto committed = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*committed, cookie));

    // Test: Attempt to perform a second SyncDelete (different cookie i.e.
    // client).
    MockCookie secondClient;
    uint64_t cas = 0;
    mutation_descr_t mutInfo;
    cb::durability::Requirements reqs{cb::durability::Level::Majority, {}};
    EXPECT_EQ(ENGINE_SYNC_WRITE_IN_PROGRESS,
              store->deleteItem(
                      key, cas, vbid, &secondClient, reqs, nullptr, mutInfo));
}

TEST_P(DurabilityBucketTest, TakeoverSendsDurabilityAmbiguous) {
    setVBucketToActiveWithValidTopology();

    // Make pending
    auto key = makeStoredDocKey("key");
    using namespace cb::durability;
    auto pending = makePendingItem(key, "value");

    // Store it
    EXPECT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

    // We don't send EWOULDBLOCK to clients
    auto mockCookie = cookie_to_mock_object(cookie);
    EXPECT_EQ(ENGINE_SUCCESS, mockCookie->status);

    // Set state to dead
    EXPECT_EQ(ENGINE_SUCCESS, store->setVBucketState(vbid, vbucket_state_dead));

    // We have set state to dead but we have not yet run the notification task
    EXPECT_EQ(ENGINE_SUCCESS, mockCookie->status);

    auto& lpAuxioQ = *task_executor->getLpTaskQ()[NONIO_TASK_IDX];
    runNextTask(lpAuxioQ);

    // We should have told client the SyncWrite is ambiguous
    EXPECT_EQ(ENGINE_SYNC_WRITE_AMBIGUOUS, mockCookie->status);
}

TEST_F(DurabilityRespondAmbiguousTest, RespondAmbiguousNotificationDeadLock) {
    // Anecdotally this takes between 0.5 and 1s to run on my dev machine
    // (MB Pro 2017 - PCIe SSD). The test typically hits the issue on the 1st
    // run but sometimes takes up to 5. I didn't want to increase the number
    // of iterations as the test will obviously take far longer to run. If
    // this test ever causes a timeout - a deadlock issue (probably in the
    // RespondAmbiguousNotification task) is present.
    for (int i = 0; i < 100; i++) {
        KVBucketTest::SetUp();

        EXPECT_EQ(ENGINE_SUCCESS,
                  store->setVBucketState(
                          vbid,
                          vbucket_state_active,
                          {{"topology",
                            nlohmann::json::array({{"active", "replica"}})}}));

        auto key = makeStoredDocKey("key");
        using namespace cb::durability;
        auto pending = makePendingItem(key, "value");

        // Store it
        EXPECT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

        // We don't send EWOULDBLOCK to clients
        auto mockCookie = cookie_to_mock_object(cookie);
        EXPECT_EQ(ENGINE_SUCCESS, mockCookie->status);

        // Set state to dead - this will schedule the task
        EXPECT_EQ(ENGINE_SUCCESS,
                  store->setVBucketState(vbid, vbucket_state_dead));

        // Deleting the vBucket will set the deferred deletion flag that
        // causes deadlock when the RespondAmbiguousNotification task is
        // destroyed as part of shutdown but is the last owner of the vBucket
        // (attempts to schedule destruction and tries to recursively lock a
        // mutex)
        {
            auto ptr = store->getVBucket(vbid);
            store->deleteVBucket(vbid, nullptr);
        }

        destroy_mock_event_callbacks();
        engine->getDcpConnMap().manageConnections();

        // Should deadlock here in ~SynchronousEPEngine
        engine.reset();

        // The RespondAmbiguousNotification task requires our cookie to still be
        // valid so delete it only after it has been destroyed
        destroy_mock_cookie(cookie);

        ExecutorPool::shutdown();
    }
}

// Test that if a SyncWrite times out, then a subsequent SyncWrite which
// _should_ fail does indeed fail.
// (Regression test for part of MB-34367 - after using notify_IO_complete
// to report the SyncWrite was timed out with status eambiguous, the outstanding
// cookie context was not correctly cleared.
TEST_P(DurabilityBucketTest, MutationAfterTimeoutCorrect) {
    setVBucketToActiveWithValidTopology();

    // Setup: make pending item and store it; then abort it (at VBucket) level.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    uint64_t cas;
    ASSERT_EQ(ENGINE_EWOULDBLOCK,
              engine->store(cookie,
                            pending.get(),
                            cas,
                            OPERATION_SET,
                            pending->getDurabilityReqs(),
                            DocumentState::Alive));
    ASSERT_TRUE(engine->getEngineSpecific(cookie))
            << "Expected engine specific to be set for cookie after "
               "EWOULDBLOCK";

    auto& vb = *store->getVBucket(vbid);
    ASSERT_EQ(ENGINE_SUCCESS,
              vb.abort(key,
                       pending->getBySeqno(),
                       {},
                       vb.lockCollections(key),
                       cookie));

    // Test: Attempt another SyncWrite, which _should_ fail (in this case just
    // use replace against the same non-existent key).
    ASSERT_EQ(ENGINE_KEY_ENOENT,
              engine->store(cookie,
                            pending.get(),
                            cas,
                            OPERATION_REPLACE,
                            pending->getDurabilityReqs(),
                            DocumentState::Alive));
}

TEST_P(DurabilityBucketTest,
       TakeoverDestinationHandlesPreparedSyncWriteMajority) {
    testTakeoverDestinationHandlesPreparedSyncWrites(
            cb::durability::Level::Majority);
}

TEST_P(DurabilityBucketTest,
       TakeoverDestinationHandlesPreparedyncWritePersistToMajority) {
    testTakeoverDestinationHandlesPreparedSyncWrites(
            cb::durability::Level::PersistToMajority);
}

// MB-34453: Block SyncWrites if there are more than this many replicas in the
// chain as we cannot guarantee no dataloss in a particular failover+rollback
// scenario.
TEST_P(DurabilityBucketTest, BlockSyncWritesIfMoreThan2Replicas) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology",
              nlohmann::json::array(
                      {{"active", "replica1", "replica2", "replica3"}})}});

    auto pre1 = makePendingItem(makeStoredDocKey("set"), "value");
    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->set(*pre1, cookie));

    auto pre2 = makePendingItem(makeStoredDocKey("add"), "value");
    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->add(*pre2, cookie));

    auto pre3 = makePendingItem(makeStoredDocKey("replace"), "value");
    EXPECT_EQ(ENGINE_DURABILITY_IMPOSSIBLE, store->replace(*pre3, cookie));
}

class FailOnExpiryCallback : public Callback<Item&, time_t&> {
public:
    void callback(Item& item, time_t& time) override {
        FAIL() << "Item was expired, nothing should be eligible for expiry";
    }
};

TEST_P(DurabilityEPBucketTest, DoNotExpirePendingItem) {
    /* MB-34768: the expiry time field of deletes has two uses - expiry time,
     * and deletion time (for use by the tombstone purger). This is true for
     * SyncDelete Prepares do too - BUT SyncDelete Prepares are not treated
     * as deleted (they are not tombstones yet) but are ALSO not eligible
     * for expiry, despite the expiry time field being set. Check that
     * compaction does not misinterpret the state of the prepare and try to
     * expire it.
     */
    setVBucketToActiveWithValidTopology();
    using namespace cb::durability;

    auto key1 = makeStoredDocKey("key1");
    auto req = Requirements(Level::Majority, Timeout(1000));

    auto key = makeStoredDocKey("key");
    // Store item normally
    queued_item qi{new Item(key, 0, 0, "value", 5)};
    EXPECT_EQ(ENGINE_SUCCESS, store->set(*qi, cookie));

    // attempt to sync delete it
    auto pending = makePendingItem(key, "value", req);
    pending->setDeleted(DeleteSource::Explicit);
    // expiry time is set *now*
    EXPECT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

    flushVBucketToDiskIfPersistent(vbid, 2);

    CompactionConfig config;
    compaction_ctx cctx(config, 0);
    cctx.curr_time = 0; // not used??

    cctx.expiryCallback = std::make_shared<FailOnExpiryCallback>();

    // Jump slightly forward, to ensure the new current time
    // is > expiry time of the delete
    TimeTraveller tt(1);

    auto* kvstore = store->getOneRWUnderlying();

    // Compact. Nothing should be expired
    EXPECT_TRUE(kvstore->compactDB(&cctx));

    // Check the committed item on disk.
    auto gv = kvstore->get(DiskDocKey(key), Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_EQ(*qi, *gv.item);

    // Check the Prepare on disk
    DiskDocKey prefixedKey(key, true /*prepare*/);
    gv = kvstore->get(prefixedKey, Vbid(0));
    EXPECT_EQ(ENGINE_SUCCESS, gv.getStatus());
    EXPECT_TRUE(gv.item->isPending());
    EXPECT_TRUE(gv.item->isDeleted());
}

template <typename F>
void DurabilityEphemeralBucketTest::testPurgeCompletedPrepare(F& func) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    auto& vb = *store->getVBucket(vbid);

    // prepare SyncWrite and commit.
    auto key = makeStoredDocKey("key");
    auto pending = makePendingItem(key, "value");
    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));

    EXPECT_EQ(ENGINE_SUCCESS, func(vb, key));

    EXPECT_EQ(1, vb.ht.getNumPreparedSyncWrites());

    TimeTraveller avenger(10000000);

    EphemeralVBucket::HTTombstonePurger purger(0);
    EphemeralVBucket& evb = dynamic_cast<EphemeralVBucket&>(vb);
    purger.setCurrentVBucket(evb);
    evb.ht.visit(purger);

    EXPECT_EQ(0, vb.ht.getNumPreparedSyncWrites());
}

TEST_P(DurabilityEphemeralBucketTest, PurgeCompletedPrepare) {
    auto op = [this](VBucket& vb, StoredDocKey key) -> ENGINE_ERROR_CODE {
        return vb.commit(key,
                         2 /*prepareSeqno*/,
                         {} /*commitSeqno*/,
                         vb.lockCollections(key));
    };
    testPurgeCompletedPrepare(op);
}

TEST_P(DurabilityEphemeralBucketTest, PurgeCompletedAbort) {
    auto op = [this](VBucket& vb, StoredDocKey key) -> ENGINE_ERROR_CODE {
        return vb.abort(key,
                        1 /*prepareSeqno*/,
                        {} /*abortSeqno*/,
                        vb.lockCollections(key));
    };
    testPurgeCompletedPrepare(op);
}

// Test to confirm that prepares in state PrepareCommitted are not expired
TEST_P(DurabilityEphemeralBucketTest, CompletedPreparesNotExpired) {
    setVBucketStateAndRunPersistTask(
            vbid,
            vbucket_state_active,
            {{"topology", nlohmann::json::array({{"active", "replica"}})}});
    const Vbid active_vb = Vbid(0);
    auto vb = engine->getVBucket(active_vb);

    const std::string value(1024, 'x'); // 1KB value to use for documents.

    auto key = makeStoredDocKey("key");
    auto item = makePendingItem(key, "value");

    using namespace std::chrono;
    auto expiry = system_clock::now() + seconds(1);

    item->setExpTime(system_clock::to_time_t(expiry));

    EXPECT_EQ(ENGINE_EWOULDBLOCK, store->set(*item, cookie));

    ASSERT_EQ(ENGINE_SUCCESS,
              vb->commit(key,
                         1 /*prepareSeqno*/,
                         {} /*commitSeqno*/,
                         vb->lockCollections(key)));

    TimeTraveller hgwells(10);

    std::shared_ptr<std::atomic<bool>> available;

    Configuration& cfg = engine->getConfiguration();
    std::unique_ptr<MockPagingVisitor> pv = std::make_unique<MockPagingVisitor>(
            *engine->getKVBucket(),
            engine->getEpStats(),
            -1,
            available,
            EXPIRY_PAGER,
            false,
            1,
            VBucketFilter(),
            nullptr,
            true,
            cfg.getItemEvictionAgePercentage(),
            cfg.getItemEvictionFreqCounterAgeThreshold());

    {
        auto pending = vb->ht.findForCommit(key).pending;
        ASSERT_TRUE(pending);
        ASSERT_TRUE(pending->isCompleted());
        ASSERT_EQ(pending->getCommitted(), CommittedState::PrepareCommitted);
    }

    pv->setCurrentBucket(vb);
    for (int ii = 0; ii <= Item::initialFreqCount; ii++) {
        pv->setFreqCounterThreshold(0);
        vb->ht.visit(*pv);
        pv->update();
    }

    {
        auto pending = vb->ht.findForCommit(key).pending;
        EXPECT_TRUE(pending);
        EXPECT_TRUE(pending->isCompleted());
    }
}

// Highlighted in MB-34997 was a situation where a vb state change meant that
// the new PDM had no knowledge of outstanding prepares that existed before the
// state change. This is fixed in VBucket by transferring the outstanding
// prepares from the ADM to the new PDM in such a switch over. This test
// demonstrates the issue and exercises the fix.
TEST_P(DurabilityBucketTest, ActiveToReplicaAndCommit) {
    setVBucketToActiveWithValidTopology();

    // seqno:1 A prepare, that does not commit yet.
    auto key = makeStoredDocKey("crikey");
    auto pending = makePendingItem(key, "pending");

    ASSERT_EQ(ENGINE_EWOULDBLOCK, store->set(*pending, cookie));
    ASSERT_EQ(
            ENGINE_EWOULDBLOCK,
            store->set(*makePendingItem(makeStoredDocKey("crikey2"), "value2"),
                       cookie));

    flushVBucketToDiskIfPersistent(vbid, 2);

    // Now switch over to being a replica, via dead for realism
    setVBucketStateAndRunPersistTask(vbid, vbucket_state_dead, {});

    setVBucketStateAndRunPersistTask(vbid, vbucket_state_replica, {});
    auto& vb = *store->getVBucket(vbid);

    // Now drive the VB as if a passive stream is receiving data.
    vb.checkpointManager->createSnapshot(
            1, 3, {} /*HCS*/, CheckpointType::Memory);

    // seqno:3 A new prepare
    auto key1 = makeStoredDocKey("crikey3");
    auto pending3 = makePendingItem(
            key1, "pending", {cb::durability::Level::Majority, {5000}});
    pending3->setCas(1);
    pending3->setBySeqno(3);
    EXPECT_EQ(ENGINE_SUCCESS, store->prepare(*pending3, cookie));
    // Trigger update of HPS (normally called by PassiveStream).
    vb.notifyPassiveDMOfSnapEndReceived(3);

    // seqno:4 the prepare at seqno:1 is committed
    vb.checkpointManager->createSnapshot(
            4, 4, {} /*HCS*/, CheckpointType::Memory);
    ASSERT_EQ(ENGINE_SUCCESS, vb.commit(key, 1, 4, vb.lockCollections(key)));
}

// Test cases which run against all persistent storage backends.
INSTANTIATE_TEST_CASE_P(
        AllBackends,
        DurabilityEPBucketTest,
        STParameterizedBucketTest::persistentAllBackendsConfigValues(),
        STParameterizedBucketTest::PrintToStringParamName);

// Test cases which run against all ephemeral.
INSTANTIATE_TEST_CASE_P(AllBackends,
                        DurabilityEphemeralBucketTest,
                        STParameterizedBucketTest::ephConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);

// Test cases which run against all configurations.
INSTANTIATE_TEST_CASE_P(AllBackends,
                        DurabilityBucketTest,
                        STParameterizedBucketTest::allConfigValues(),
                        STParameterizedBucketTest::PrintToStringParamName);
