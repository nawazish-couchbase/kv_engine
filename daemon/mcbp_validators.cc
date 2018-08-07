/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
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

#include "config.h"

#include "connection.h"
#include "mcbp_validators.h"
#include <memcached/dcp.h>
#include <memcached/protocol_binary.h>
#include <platform/compress.h>
#include <platform/string.h>

#include "buckets.h"
#include "memcached.h"
#include "subdocument_validators.h"
#include "xattr/utils.h"

static inline bool may_accept_xattr(const Cookie& cookie) {
    auto* req = static_cast<protocol_binary_request_header*>(
            cookie.getPacketAsVoidPtr());
    if (mcbp::datatype::is_xattr(req->request.datatype)) {
        return cookie.getConnection().isXattrEnabled();
    }

    return true;
}

bool is_document_key_valid(const Cookie& cookie) {
    const auto& req = cookie.getRequest(Cookie::PacketContent::Header);
    if (cookie.getConnection().isCollectionsSupported()) {
        const auto& key = req.getKey();
        auto stopByte = cb::mcbp::unsigned_leb128_get_stop_byte_index(key);
        // 1. CID is leb128 encode, key must then be 1 byte of key and 1 byte of
        //    leb128 minimum
        // 2. Secondly - require that the leb128 and key are encoded, i.e. we
        //    expect that the leb128 stop byte is not the last byte of the key.
        return req.getKeylen() > 1 && stopByte && (key.size() - 1) > *stopByte;
    }
    return req.getKeylen() > 0;
}

static inline bool may_accept_dcp_deleteV2(const Cookie& cookie) {
    return cookie.getConnection().isDcpDeleteV2();
}

static inline std::string get_peer_description(const Cookie& cookie) {
    return cookie.getConnection().getDescription();
}

enum class ExpectedKeyLen { Zero, NonZero, Any };
enum class ExpectedValueLen { Zero, NonZero, Any };
enum class ExpectedCas { Set, NotSet, Any };

/**
 * Verify the header meets basic sanity checks and fields length
 * match the provided expected lengths.
 */
static bool verify_header(
        Cookie& cookie,
        uint8_t expected_extlen,
        ExpectedKeyLen expected_keylen,
        ExpectedValueLen expected_valuelen,
        ExpectedCas expected_cas = ExpectedCas::Any,
        uint8_t expected_datatype_mask = mcbp::datatype::highest) {
    const auto& header = cookie.getHeader();

    if (!header.isValid()) {
        cookie.setErrorContext("Request header invalid");
        return false;
    }
    if (!mcbp::datatype::is_valid(header.getDatatype())) {
        cookie.setErrorContext("Request datatype invalid");
        return false;
    }

    if ((expected_extlen == 0) && (header.getExtlen() != 0)) {
        cookie.setErrorContext("Request must not include extras");
        return false;
    }
    if ((expected_extlen != 0) && (header.getExtlen() != expected_extlen)) {
        cookie.setErrorContext("Request must include extras of length " +
                               std::to_string(expected_extlen));
        return false;
    }

    switch (expected_keylen) {
    case ExpectedKeyLen::Zero:
        if (header.getKeylen() != 0) {
            cookie.setErrorContext("Request must not include key");
            return false;
        }
        break;
    case ExpectedKeyLen::NonZero:
        if (header.getKeylen() == 0) {
            cookie.setErrorContext("Request must include key");
            return false;
        }
        break;
    case ExpectedKeyLen::Any:
        break;
    }

    uint32_t valuelen =
            header.getBodylen() - header.getKeylen() - header.getExtlen();
    switch (expected_valuelen) {
    case ExpectedValueLen::Zero:
        if (valuelen != 0) {
            cookie.setErrorContext("Request must not include value");
            return false;
        }
        break;
    case ExpectedValueLen::NonZero:
        if (valuelen == 0) {
            cookie.setErrorContext("Request must include value");
            return false;
        }
        break;
    case ExpectedValueLen::Any:
        break;
    }

    switch (expected_cas) {
    case ExpectedCas::NotSet:
        if (header.getCas() != 0) {
            cookie.setErrorContext("Request CAS must not be set");
            return false;
        }
        break;
    case ExpectedCas::Set:
        if (header.getCas() == 0) {
            cookie.setErrorContext("Request CAS must be set");
            return false;
        }
        break;
    case ExpectedCas::Any:
        break;
    }

    if ((~expected_datatype_mask) & header.getDatatype()) {
        cookie.setErrorContext("Request datatype invalid");
        return false;
    }

    return true;
}

/******************************************************************************
 *                         Package validators                                 *
 *****************************************************************************/

/**
 * Verify that the cookie meets the common DCP restrictions:
 *
 * a) The connected engine supports DCP
 * b) The connection cannot be set into the unordered execution mode.
 *
 * In the future it should be extended to verify that the various DCP
 * commands is only sent on a connection which is set up as a DCP
 * connection (except the initial OPEN etc)
 *
 * @param cookie The command cookie
 */
static protocol_binary_response_status verify_common_dcp_restrictions(
        Cookie& cookie) {
    auto* dcp = cookie.getConnection().getBucket().getDcpIface();
    if (!dcp) {
        // The attached bucket does not support DCP
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    const auto& connection = cookie.getConnection();
    if (connection.allowUnorderedExecution()) {
        LOG_WARNING(
                "DCP on a connection with unordered execution is currently "
                "not supported: {}",
                get_peer_description(cookie));
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status dcp_open_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_open*>(
            cookie.getPacketAsVoidPtr());

    if (!verify_header(cookie,
                       8,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // If there's a value, then collections must be enabled
    const uint32_t valuelen = ntohl(req->message.header.request.bodylen) -
                              req->message.header.request.extlen -
                              ntohs(req->message.header.request.keylen);

    if (!cookie.getConnection().isCollectionsSupported() && valuelen) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    const auto mask = DCP_OPEN_PRODUCER | DCP_OPEN_NOTIFIER |
                      DCP_OPEN_INCLUDE_XATTRS | DCP_OPEN_NO_VALUE |
                      DCP_OPEN_INCLUDE_DELETE_TIMES;

    const auto flags = ntohl(req->message.body.flags);

    if (flags & ~mask) {
        LOG_INFO(
                "Client trying to open dcp stream with unknown flags ({:x}) {}",
                flags,
                get_peer_description(cookie));
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if ((flags & DCP_OPEN_NOTIFIER) && (flags & ~DCP_OPEN_NOTIFIER)) {
        LOG_INFO(
                "Invalid flags combination ({:x}) specified for a DCP "
                "consumer {}",
                flags,
                get_peer_description(cookie));
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_add_stream_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_add_stream*>(
            cookie.getPacketAsVoidPtr());

    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    const auto flags = ntohl(req->message.body.flags);
    const auto mask = DCP_ADD_STREAM_FLAG_TAKEOVER |
                      DCP_ADD_STREAM_FLAG_DISKONLY |
                      DCP_ADD_STREAM_FLAG_LATEST |
                      DCP_ADD_STREAM_ACTIVE_VB_ONLY;

    if (flags & ~mask) {
        if (flags & DCP_ADD_STREAM_FLAG_NO_VALUE) {
            // MB-22525 The NO_VALUE flag should be passed to DCP_OPEN
            LOG_INFO("Client trying to add stream with NO VALUE {}",
                     get_peer_description(cookie));
        } else {
            LOG_INFO("Client trying to add stream with unknown flags ({:x}) {}",
                     flags,
                     get_peer_description(cookie));
        }
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_close_stream_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_get_failover_log_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_stream_req_validator(Cookie& cookie)
{
    constexpr uint8_t expected_extlen =
            5 * sizeof(uint64_t) + 2 * sizeof(uint32_t);

    if (!verify_header(cookie,
                       expected_extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_stream_end_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_snapshot_marker_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       20,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_system_event_validator(
        Cookie& cookie) {
    // keylen + bodylen > ??
    auto req = static_cast<protocol_binary_request_dcp_system_event*>(
            cookie.getPacketAsVoidPtr());

    if (!verify_header(
                cookie,
                protocol_binary_request_dcp_system_event::getExtrasLength(),
                ExpectedKeyLen::Any,
                ExpectedValueLen::Any)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (!mcbp::systemevent::validate(ntohl(req->message.body.event))) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static bool is_valid_xattr_blob(const protocol_binary_request_header& header) {
    const uint32_t extlen{header.request.extlen};
    const uint32_t keylen{ntohs(header.request.keylen)};
    const uint32_t bodylen{ntohl(header.request.bodylen)};

    auto* ptr = reinterpret_cast<const char*>(header.bytes);
    ptr += sizeof(header.bytes) + extlen + keylen;

    cb::compression::Buffer buffer;
    cb::const_char_buffer xattr{ptr, bodylen - keylen - extlen};
    if (mcbp::datatype::is_snappy(header.request.datatype)) {
        // Inflate the xattr data and validate that.
        if (!cb::compression::inflate(
                    cb::compression::Algorithm::Snappy, xattr, buffer)) {
            return false;
        }
        xattr = buffer;
    }

    return cb::xattr::validate(xattr);
}

static protocol_binary_response_status dcp_mutation_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_mutation*>(
            cookie.getPacketAsVoidPtr());
    const auto datatype = req->message.header.request.datatype;

    if (!verify_header(cookie,
                       protocol_binary_request_dcp_mutation::getExtrasLength(),
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (!may_accept_xattr(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (mcbp::datatype::is_xattr(datatype) &&
        !is_valid_xattr_blob(req->message.header)) {
        return PROTOCOL_BINARY_RESPONSE_XATTR_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

/// @return true if the datatype is valid for a deletion
static bool valid_dcp_delete_datatype(protocol_binary_datatype_t datatype) {
    // MB-29040: Allowing xattr + JSON. A bug in the producer means
    // it may send XATTR|JSON (with snappy possible). These are now allowed
    // so rebalance won't be failed and the consumer will sanitise the faulty
    // documents.
    std::array<const protocol_binary_datatype_t, 5> valid = {
            {PROTOCOL_BINARY_RAW_BYTES,
             PROTOCOL_BINARY_DATATYPE_XATTR,
             PROTOCOL_BINARY_DATATYPE_XATTR | PROTOCOL_BINARY_DATATYPE_SNAPPY,
             PROTOCOL_BINARY_DATATYPE_XATTR | PROTOCOL_BINARY_DATATYPE_JSON,
             PROTOCOL_BINARY_DATATYPE_XATTR | PROTOCOL_BINARY_DATATYPE_SNAPPY |
                     PROTOCOL_BINARY_DATATYPE_JSON}};
    for (auto d : valid) {
        if (datatype == d) {
            return true;
        }
    }
    return false;
}

static protocol_binary_response_status dcp_deletion_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_deletion*>(
            cookie.getPacketAsVoidPtr());

    const uint8_t expectedExtlen =
            may_accept_dcp_deleteV2(cookie)
                    ? protocol_binary_request_dcp_deletion_v2::extlen
                    : protocol_binary_request_dcp_deletion::extlen;

    if (!verify_header(cookie,
                       expectedExtlen,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!valid_dcp_delete_datatype(req->message.header.request.datatype)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_expiration_validator(Cookie& cookie)
{
    if (!verify_header(
                cookie,
                gsl::narrow<uint8_t>(protocol_binary_request_dcp_expiration::
                                             getExtrasLength()),
                ExpectedKeyLen::NonZero,
                ExpectedValueLen::Zero,
                ExpectedCas::Any,
                PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_set_vbucket_state_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_dcp_set_vbucket_state*>(
            cookie.getPacketAsVoidPtr());

    if (!verify_header(cookie,
                       1,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (req->message.body.state < 1 || req->message.body.state > 4) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_noop_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_buffer_acknowledgement_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status dcp_control_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::NonZero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return verify_common_dcp_restrictions(cookie);
}

static protocol_binary_response_status revoke_user_permissions_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status configuration_refresh_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status rbac_provider_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status verbosity_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status hello_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t len = ntohl(req->message.header.request.bodylen);
    len -= ntohs(req->message.header.request.keylen);

    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Any,
                       ExpectedValueLen::Any,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if ((len % 2) != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status version_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status quit_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status sasl_list_mech_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status sasl_auth_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status noop_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status flush_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint8_t extlen = req->message.header.request.extlen;

    if (extlen != 0 && extlen != 4) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    // We've already checked extlen so pass actual extlen as expected extlen
    if (!verify_header(cookie,
                       extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen == 4) {
        // We don't support delayed flush anymore
        auto* req = reinterpret_cast<protocol_binary_request_flush*>(
                cookie.getPacketAsVoidPtr());
        if (req->message.body.expiration != 0) {
            return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
        }
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status add_validator(Cookie& cookie)
{
    constexpr uint8_t expected_datatype_mask = PROTOCOL_BINARY_RAW_BYTES |
                                               PROTOCOL_BINARY_DATATYPE_JSON |
                                               PROTOCOL_BINARY_DATATYPE_SNAPPY;
    /* Must have extras and key, may have value */
    if (!verify_header(cookie,
                       8,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::NotSet,
                       expected_datatype_mask)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_replace_validator(Cookie& cookie)
{
    constexpr uint8_t expected_datatype_mask = PROTOCOL_BINARY_RAW_BYTES |
                                               PROTOCOL_BINARY_DATATYPE_JSON |
                                               PROTOCOL_BINARY_DATATYPE_SNAPPY;
    /* Must have extras and key, may have value */
    if (!verify_header(cookie,
                       8,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       expected_datatype_mask)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status append_prepend_validator(Cookie& cookie)
{
    constexpr uint8_t expected_datatype_mask = PROTOCOL_BINARY_RAW_BYTES |
                                               PROTOCOL_BINARY_DATATYPE_JSON |
                                               PROTOCOL_BINARY_DATATYPE_SNAPPY;
    /* Must not have extras, must have key, may have value */
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       expected_datatype_mask)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status gat_validator(Cookie& cookie) {
    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status stat_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Any,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status arithmetic_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       20,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_cmd_timer_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       1,
                       ExpectedKeyLen::Any,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_ctrl_token_validator(Cookie& cookie)
{
    constexpr uint8_t expected_extlen = sizeof(uint64_t);

    if (!verify_header(cookie,
                       expected_extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    auto req = static_cast<protocol_binary_request_set_ctrl_token*>(
            cookie.getPacketAsVoidPtr());

    if (req->message.body.new_cas == 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_ctrl_token_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status ioctl_get_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (cookie.getHeader().getKeylen() > IOCTL_KEY_LENGTH) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status ioctl_set_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    auto req = static_cast<protocol_binary_request_ioctl_set*>(
            cookie.getPacketAsVoidPtr());
    uint16_t klen = ntohs(req->message.header.request.keylen);
    size_t vallen = ntohl(req->message.header.request.bodylen) - klen;

    if (klen > IOCTL_KEY_LENGTH || vallen > IOCTL_VAL_LENGTH) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status audit_put_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       4,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::NonZero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status audit_config_reload_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status config_reload_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status config_validate_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::NonZero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    auto& header = cookie.getHeader();
    const auto bodylen = header.getBodylen();

    if (bodylen > CONFIG_VALIDATE_MAX_LENGTH) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status observe_seqno_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (cookie.getHeader().getBodylen() != 8) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_adjusted_time_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status set_drift_counter_state_validator(Cookie& cookie)
{
    constexpr uint8_t expected_extlen = sizeof(uint8_t) + sizeof(int64_t);

    if (!verify_header(cookie,
                       expected_extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

/**
 * The create bucket contains message have the following format:
 *    key: bucket name
 *    body: module\nconfig
 */
static protocol_binary_response_status create_bucket_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::NonZero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (cookie.getHeader().getKeylen() > MAX_BUCKET_NAME_LENGTH) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status list_bucket_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_bucket_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status select_bucket_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Any,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (cookie.getHeader().getKeylen() > 1023) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_all_vb_seqnos_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_get_all_vb_seqnos*>(
            cookie.getPacketAsVoidPtr());
    uint8_t extlen = req->message.header.request.extlen;

    // We check extlen below so pass actual extlen as expected_extlen to bypass
    // the check in verify_header
    if (!verify_header(cookie,
                       extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen != 0) {
        // extlen is optional, and if non-zero it contains the vbucket
        // state to report
        if (extlen != sizeof(vbucket_state_t)) {
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
        vbucket_state_t state;
        memcpy(&state, &req->message.body.state, sizeof(vbucket_state_t));
        state = static_cast<vbucket_state_t>(ntohl(state));
        if (!is_valid_vbucket_state_t(state)) {
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status shutdown_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Set,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}



static protocol_binary_response_status get_meta_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t extlen = req->message.header.request.extlen;

    // We check extlen below so pass actual extlen as expected_extlen to bypass
    // the check in verify_header
    if (!verify_header(cookie,
                       extlen,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (extlen > 1) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (extlen == 1) {
        const uint8_t* extdata = req->bytes + sizeof(req->bytes);
        if (*extdata > 2) {
            // 1 == return conflict resolution mode
            // 2 == return datatype
            return PROTOCOL_BINARY_RESPONSE_EINVAL;
        }
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status mutate_with_meta_validator(Cookie& cookie) {
    auto req = static_cast<protocol_binary_request_get_meta*>(
            cookie.getPacketAsVoidPtr());

    const uint32_t extlen = req->message.header.request.extlen;
    const auto datatype = req->message.header.request.datatype;

    // We check extlen below so pass actual extlen as expected_extlen to bypass
    // the check in verify_header
    if (!verify_header(cookie,
                       extlen,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Any)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie) || !may_accept_xattr(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // revid_nbytes, flags and exptime is mandatory fields.. and we need a key
    // extlen, the size dicates what is encoded.
    switch (extlen) {
    case 24: // no nmeta and no options
    case 26: // nmeta
    case 28: // options (4-byte field)
    case 30: // options and nmeta (options followed by nmeta)
        break;
    default:
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (mcbp::datatype::is_xattr(datatype) &&
        !is_valid_xattr_blob(req->message.header)) {
        return PROTOCOL_BINARY_RESPONSE_XATTR_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_errmap_validator(Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Any,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    const auto& hdr = *static_cast<const protocol_binary_request_header*>(
            cookie.getPacketAsVoidPtr());

    if (hdr.request.vbucket != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (hdr.request.getBodylen() != 2) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status get_locked_validator(Cookie& cookie)
{
    auto req = static_cast<protocol_binary_request_no_extras*>(
            cookie.getPacketAsVoidPtr());
    uint32_t extlen = req->message.header.request.extlen;

    // We check extlen below so pass actual extlen as expected extlen to bypass
    // the check in verify header
    if (!verify_header(cookie,
                       extlen,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    if (!is_document_key_valid(cookie) || (extlen != 0 && extlen != 4)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status unlock_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Set,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status evict_key_validator(Cookie& cookie)
{
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::NonZero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (!is_document_key_valid(cookie)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status collections_set_manifest_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::NonZero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
    if (cookie.getHeader().getRequest().getVBucket() != 0) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    auto* engine = cookie.getConnection().getBucket().getEngine();
    if (engine == nullptr || engine->collections.set_manifest == nullptr) {
        // The attached bucket does not support collections
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status collections_get_manifest_validator(
        Cookie& cookie) {
    if (!verify_header(cookie,
                       0,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::Any,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // We could do these tests before checking the packet, but
    // it feels cleaner to validate the packet first.
    auto* engine = cookie.getConnection().getBucket().getEngine();
    if (engine == nullptr || engine->collections.get_manifest == nullptr) {
        // The attached bucket does not support collections
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status adjust_timeofday_validator(Cookie& cookie)
{
    constexpr uint8_t expected_extlen = sizeof(uint64_t) + sizeof(uint8_t);

    if (!verify_header(cookie,
                       expected_extlen,
                       ExpectedKeyLen::Zero,
                       ExpectedValueLen::Zero,
                       ExpectedCas::NotSet,
                       PROTOCOL_BINARY_RAW_BYTES)) {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }

    // The method should only be available for unit tests
    if (getenv("MEMCACHED_UNIT_TESTS") == nullptr) {
        return PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

void McbpValidatorChains::initializeMcbpValidatorChains(McbpValidatorChains& chains) {
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_OPEN, dcp_open_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_ADD_STREAM, dcp_add_stream_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM, dcp_close_stream_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER, dcp_snapshot_marker_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_DELETION, dcp_deletion_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_EXPIRATION, dcp_expiration_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG, dcp_get_failover_log_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_MUTATION, dcp_mutation_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE, dcp_set_vbucket_state_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_NOOP, dcp_noop_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT, dcp_buffer_acknowledgement_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_CONTROL, dcp_control_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_STREAM_END, dcp_stream_end_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_STREAM_REQ, dcp_stream_req_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT, dcp_system_event_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ISASL_REFRESH, configuration_refresh_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SSL_CERTS_REFRESH, configuration_refresh_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_VERBOSITY, verbosity_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_HELLO, hello_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_VERSION, version_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_QUIT, quit_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_QUITQ, quit_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_LIST_MECHS, sasl_list_mech_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_AUTH, sasl_auth_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SASL_STEP, sasl_auth_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_NOOP, noop_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_FLUSH, flush_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_FLUSHQ, flush_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETQ, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETK, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETKQ, get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GAT, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GATQ, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_TOUCH, gat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETE, delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETEQ, delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_STAT, stat_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_INCREMENT, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_INCREMENTQ, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DECREMENT, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DECREMENTQ, arithmetic_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_CMD_TIMER, get_cmd_timer_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN, set_ctrl_token_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_CTRL_TOKEN, get_ctrl_token_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_IOCTL_GET, ioctl_get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_IOCTL_SET, ioctl_set_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_AUDIT_PUT, audit_put_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_AUDIT_CONFIG_RELOAD, audit_config_reload_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_CONFIG_RELOAD,
                       config_reload_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_CONFIG_VALIDATE,
                       config_validate_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SHUTDOWN, shutdown_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_OBSERVE_SEQNO, observe_seqno_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ADJUSTED_TIME, get_adjusted_time_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_DRIFT_COUNTER_STATE, set_drift_counter_state_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_GET, subdoc_get_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_EXISTS, subdoc_exists_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD, subdoc_dict_add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DICT_UPSERT, subdoc_dict_upsert_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_DELETE, subdoc_delete_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_REPLACE, subdoc_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST, subdoc_array_push_last_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST, subdoc_array_push_first_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT, subdoc_array_insert_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE, subdoc_array_add_unique_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_COUNTER, subdoc_counter_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP, subdoc_multi_lookup_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION, subdoc_multi_mutation_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SUBDOC_GET_COUNT, subdoc_get_count_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_SETQ, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADDQ, add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADD, add_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_REPLACEQ, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_REPLACE, set_replace_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_APPENDQ, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_APPEND, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_PREPENDQ, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_PREPEND, append_prepend_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_CREATE_BUCKET, create_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_LIST_BUCKETS, list_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELETE_BUCKET, delete_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SELECT_BUCKET, select_bucket_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ALL_VB_SEQNOS, get_all_vb_seqnos_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_EVICT_KEY, evict_key_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_GET_META, get_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GETQ_META, get_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SET_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_SETQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADD_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_ADDQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DEL_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_DELQ_WITH_META, mutate_with_meta_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_ERROR_MAP, get_errmap_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_LOCKED, get_locked_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_UNLOCK_KEY, unlock_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_REVOKE_USER_PERMISSIONS,
                       revoke_user_permissions_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_RBAC_REFRESH, configuration_refresh_validator);
    chains.push_unique(uint8_t(cb::mcbp::ClientOpcode::RbacProvider),
                       rbac_provider_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_GET_FAILOVER_LOG,
                       dcp_get_failover_log_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_COLLECTIONS_SET_MANIFEST,
                       collections_set_manifest_validator);
    chains.push_unique(PROTOCOL_BINARY_CMD_COLLECTIONS_GET_MANIFEST,
                       collections_get_manifest_validator);

    chains.push_unique(PROTOCOL_BINARY_CMD_ADJUST_TIMEOFDAY,
                       adjust_timeofday_validator);
}
