/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include "ep_types.h"

#include <folly/lang/Assume.h>
#include <optional>
#include <ostream>

bool isDiskCheckpointType(CheckpointType type) {
    return type == CheckpointType::InitialDisk || type == CheckpointType::Disk;
}

CheckpointType getSuperCheckpointType(CheckpointType type) {
    switch (type) {
    // Supertypes.
    case CheckpointType::Disk:
    case CheckpointType::Memory:
        return type;
    // Subtypes.
    case CheckpointType::InitialDisk:
        return CheckpointType::Disk;
    }
    folly::assume_unreachable();
}

GenerateBySeqno getGenerateBySeqno(const OptionalSeqno& seqno) {
    return seqno ? GenerateBySeqno::No : GenerateBySeqno::Yes;
}

std::string to_string(GenerateBySeqno generateBySeqno) {
    using GenerateBySeqnoUType = std::underlying_type<GenerateBySeqno>::type;

    switch (generateBySeqno) {
    case GenerateBySeqno::Yes:
        return "Yes";
    case GenerateBySeqno::No:
        return "No";
    }
    throw std::invalid_argument(
            "to_string(GenerateBySeqno) unknown " +
            std::to_string(static_cast<GenerateBySeqnoUType>(generateBySeqno)));
}

std::string to_string(GenerateCas generateCas) {
    using GenerateByCasUType = std::underlying_type<GenerateCas>::type;

    switch (generateCas) {
    case GenerateCas::Yes:
        return "Yes";
    case GenerateCas::No:
        return "No";
    }
    throw std::invalid_argument(
            "to_string(GenerateCas) unknown " +
            std::to_string(static_cast<GenerateByCasUType>(generateCas)));
}

std::string to_string(TrackCasDrift trackCasDrift) {
    using TrackCasDriftUType = std::underlying_type<TrackCasDrift>::type;

    switch (trackCasDrift) {
    case TrackCasDrift::Yes:
        return "Yes";
    case TrackCasDrift::No:
        return "No";
    }
    throw std::invalid_argument(
            "to_string(TrackCasDrift) unknown " +
            std::to_string(static_cast<TrackCasDriftUType>(trackCasDrift)));
}

std::string to_string(CheckpointType checkpointType) {
    switch (checkpointType) {
    case CheckpointType::Disk:
        return "Disk";
    case CheckpointType::Memory:
        return "Memory";
    case CheckpointType::InitialDisk:
        return "InitialDisk";
    }
    folly::assume_unreachable();
}

std::ostream& operator<<(std::ostream& os, const EvictionPolicy& policy) {
    return os << to_string(policy);
}

std::string to_string(EvictionPolicy policy) {
    switch (policy) {
    case EvictionPolicy::Value:
        return "Value";
    case EvictionPolicy::Full:
        return "Full";
    }
    folly::assume_unreachable();
}

std::ostream& operator<<(std::ostream& os, TransferVB transfer) {
    switch (transfer) {
    case TransferVB::No:
        return os << "No";
    case TransferVB::Yes:
        return os << "Yes";
    }
    throw std::invalid_argument("operator<<(TransferVB) unknown value " +
                                std::to_string(static_cast<int>(transfer)));
}

std::ostream& operator<<(std::ostream& os, const snapshot_range_t& range) {
    return os << "{" << range.getStart() << "," << range.getEnd() << "}";
}

std::ostream& operator<<(std::ostream& os, const snapshot_info_t& info) {
    return os << "start:" << info.start << ", range:" << info.range;
}
