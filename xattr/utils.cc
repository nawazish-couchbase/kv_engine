/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */
#include <JSON_checker.h>
#include <memcached/protocol_binary.h>
#include <xattr/blob.h>
#include <xattr/key_validator.h>
#include <xattr/utils.h>

#include <unordered_set>

namespace cb::xattr {

/**
 * Small utility function to trim the blob object into a '\0' terminated
 * string.
 *
 * @param blob the blob object to operate on
 * @return the trimmed string
 * @throws std::underflow_error if there isn't a '\0' in the buffer
 */
static std::string_view trim_string(std::string_view blob) {
    auto n = blob.find_first_of('\0');
    if (n == std::string_view::npos) {
        throw std::out_of_range("trim_string: no '\\0' in the input buffer");
    }

    return blob.substr(0, n);
}

bool validate(std::string_view blob) {
    if (blob.size() < 4) {
        // we must have room for the length field
        return false;
    }

    std::size_t size;

    // You probably want to look in docs/Document.md for a detailed
    // description of the actual memory layout and why I'm adding
    // these "magic" values.
    size_t offset = 4;

    try {
        // Check that the offset of the body is within the blob (note that it
        // may be the same size as the blob if the actual data payload is empty
        size = get_body_offset(blob);
        if (size > blob.size()) {
            return false;
        }

        // @todo fix the hash thing so I can use the keybuf directly
        std::unordered_set<std::string> keys;
        JSON_checker::Validator validator;

        // Iterate over all of the KV pairs
        while (offset < size) {
            // The next pair _must_ at least have:
            //    4  byte length field,
            //    1  byte key
            //    2x 1 byte '\0'
            if (offset + 7 > size) {
                return false;
            }

            const auto kvsize = ntohl(
                    *reinterpret_cast<const uint32_t*>(blob.data() + offset));
            offset += 4;
            if (offset + kvsize > size) {
                // The kvsize exceeds the blob size
                return false;
            }

            // pick out the key
            const auto keybuf =
                    trim_string({blob.data() + offset, size - offset});
            offset += keybuf.size() + 1; // swallow the '\0'

            // Validate the key
            if (!is_valid_xattr_key(keybuf)) {
                return false;
            }

            // pick out the value
            const auto valuebuf =
                    trim_string({blob.data() + offset, size - offset});
            offset += valuebuf.size() + 1; // swallow '\0'

            // Validate the value (must be legal json)
            if (!validator.validate(valuebuf)) {
                // Failed to parse the JSON
                return false;
            }

            if (kvsize != (keybuf.size() + valuebuf.size() + 2)) {
                return false;
            }

            if (!keys.insert(std::string{keybuf}).second) {
                return false;
            }
        }
    } catch (const std::out_of_range&) {
        return false;
    }

    return offset == size;
}

// Test that a len doesn't exceed size, the idea that len is the value read from
// an xattr payload and size is the document size
static void check_len(uint32_t len, size_t size) {
    if (len > size) {
        throw std::out_of_range("xattr::utils::check_len(" +
                                std::to_string(len) + ") exceeds " +
                                std::to_string(size));
    }
}

uint32_t get_body_offset(std::string_view payload) {
    Expects(payload.size() > 0);
    const auto* lenptr = reinterpret_cast<const uint32_t*>(payload.data());
    auto len = ntohl(*lenptr);
    check_len(len, payload.size());
    return len + sizeof(uint32_t);
}

std::string_view get_body(std::string_view payload) {
    auto offset = get_body_offset(payload);
    payload.remove_prefix(offset);
    return payload;
}

size_t get_system_xattr_size(uint8_t datatype, std::string_view doc) {
    if (!::mcbp::datatype::is_xattr(datatype)) {
        return 0;
    }

    Blob blob({const_cast<char*>(doc.data()), doc.size()},
              ::mcbp::datatype::is_snappy(datatype));
    return blob.get_system_size();
}

size_t get_body_size(uint8_t datatype, std::string_view value) {
    cb::compression::Buffer uncompressed;
    if (::mcbp::datatype::is_snappy(datatype)) {
        if (!cb::compression::inflate(
                    cb::compression::Algorithm::Snappy, value, uncompressed)) {
            throw std::invalid_argument(
                    "get_body_size: Failed to inflate data");
        }
        value = uncompressed;
    }

    if (value.size() == 0) {
        return 0;
    }

    if (!::mcbp::datatype::is_xattr(datatype)) {
        return value.size();
    }

    return value.size() - get_body_offset(value);
}

std::string make_wire_encoded_string(
        const std::string& body,
        const std::unordered_map<std::string, std::string>& xattrSet) {
    Blob xattrs;
    for (const auto& [key, value] : xattrSet) {
        xattrs.set(key, value);
    }
    std::string encoded{xattrs.finalize()};
    if (!cb::xattr::validate(encoded)) {
        throw std::logic_error(
                "cb::xattr::make_wire_encoded_string Invalid xattr encoding");
    }
    encoded.append(body);
    return encoded;
}
} // namespace cb::xattr
