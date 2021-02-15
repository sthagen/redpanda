/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once
#include "bytes/iobuf.h"
#include "model/record.h"
#include "seastarx.h"
#include "utils/vint.h"

namespace storage {
class record_batch_builder {
public:
    record_batch_builder(model::record_batch_type, model::offset);
    record_batch_builder(record_batch_builder&&) = default;
    record_batch_builder(const record_batch_builder&) = delete;
    record_batch_builder& operator=(record_batch_builder&&) = default;
    record_batch_builder& operator=(const record_batch_builder&) = delete;

    virtual record_batch_builder&
    add_raw_kv(iobuf&& key, std::optional<iobuf>&& value) {
        _records.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    virtual record_batch_builder& add_raw_kw(
      iobuf&& key,
      std::optional<iobuf>&& value,
      std::vector<model::record_header> headers) {
        _records.emplace_back(
          std::move(key), std::move(value), std::move(headers));
        return *this;
    }
    virtual model::record_batch build() &&;
    virtual ~record_batch_builder();

private:
    static constexpr int64_t zero_vint_size = vint::vint_size(0);
    struct serialized_record {
        serialized_record(
          iobuf k,
          std::optional<iobuf> v,
          std::vector<model::record_header> hdrs
          = std::vector<model::record_header>())
          : key(std::move(k))
          , headers(std::move(hdrs)) {
            if (likely(v)) {
                value = std::move(*v);
                encoded_value_size = value.size_bytes();
            } else {
                encoded_value_size = -1;
            }
        }

        iobuf key;
        iobuf value;
        int32_t encoded_value_size;
        std::vector<model::record_header> headers;
    };

    uint32_t record_size(int32_t offset_delta, const serialized_record& r);

    model::record_batch_type _batch_type;
    model::offset _base_offset;
    std::vector<serialized_record> _records;
};
} // namespace storage
