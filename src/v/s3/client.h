/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/vectorizedio/redpanda/blob/master/licenses/rcl.md
 */

#pragma once

#include "http/client.h"
#include "rpc/transport.h"
#include "s3/signature.h"
#include "tristate.h"

#include <boost/property_tree/ptree_fwd.hpp>

#include <chrono>
#include <initializer_list>
#include <limits>

namespace s3 {

using access_point_uri = named_type<ss::sstring, struct s3_access_point_uri>;
using bucket_name = named_type<ss::sstring, struct s3_bucket_name>;
using object_key = named_type<std::filesystem::path, struct s3_object_key>;
using endpoint_url = named_type<ss::sstring, struct s3_endpoint_url>;
using ca_trust_file
  = named_type<std::filesystem::path, struct s3_ca_trust_file>;

struct object_tag {
    ss::sstring key;
    ss::sstring value;
};

/// List of default overrides that can be used to workaround issues
/// that can arise when we want to deal with different S3 API implementations
/// and different OS issues (like different truststore locations on different
/// Linux distributions).
struct default_overrides {
    std::optional<endpoint_url> endpoint = std::nullopt;
    std::optional<uint16_t> port = std::nullopt;
    std::optional<ca_trust_file> trust_file = std::nullopt;
    bool disable_tls = false;
};

/// S3 client configuration
struct configuration : rpc::base_transport::configuration {
    /// URI of the S3 access point
    access_point_uri uri;
    /// AWS access key
    public_key_str access_key;
    /// AWS secret key
    private_key_str secret_key;
    /// AWS region
    aws_region_name region;

    /// \brief opinionated configuraiton initialization
    /// Generates uri field from region, initializes credentials for the
    /// transport, resolves the uri to get the server_addr.
    ///
    /// \param pkey is an AWS access key
    /// \param skey is an AWS secret key
    /// \param region is an AWS region code
    /// \param overrides contains a bunch of property overrides like
    ///        non-standard SSL port and alternative location of the
    ///        truststore
    /// \return future that returns initialized configuration
    static ss::future<configuration> make_configuration(
      const public_key_str& pkey,
      const private_key_str& skey,
      const aws_region_name& region,
      const default_overrides overrides = {});
};

std::ostream& operator<<(std::ostream& o, const configuration& c);

/// Request formatter for AWS S3
class request_creator {
public:
    /// C-tor
    /// \param conf is a configuration container
    explicit request_creator(const configuration& conf);

    /// \brief Create unsigned 'PutObject' request header
    /// The payload is unsigned which means that we don't need to calculate
    /// hash from it (which don't want to do for large files).
    ///
    /// \param name is a bucket that should be used to store new object
    /// \param key is an object name
    /// \param payload_size_bytes is a size of the object in bytes
    /// \return initialized and signed http header or error
    result<http::client::request_header> make_unsigned_put_object_request(
      bucket_name const& name,
      object_key const& key,
      size_t payload_size_bytes,
      const std::vector<object_tag>& tags);

    /// \brief Create a 'GetObject' request header
    ///
    /// \param name is a bucket that has the object
    /// \param key is an object name
    /// \return initialized and signed http header or error
    result<http::client::request_header>
    make_get_object_request(bucket_name const& name, object_key const& key);

    /// \brief Create a 'DeleteObject' request header
    ///
    /// \param name is a bucket that has the object
    /// \param key is an object name
    /// \return initialized and signed http header or error
    result<http::client::request_header>
    make_delete_object_request(bucket_name const& name, object_key const& key);

    /// \brief Initialize http header for 'ListObjectsV2' request
    ///
    /// \param name of the bucket
    /// \param region to connect
    /// \param max_keys is a max number of returned objects
    /// \param offset is an offset of the first returned object
    /// \return initialized and signed http header or error
    result<http::client::request_header> make_list_objects_v2_request(
      const bucket_name& name,
      std::optional<object_key> prefix,
      std::optional<object_key> start_after,
      std::optional<size_t> max_keys);

private:
    access_point_uri _ap;
    signature_v4 _sign;
};

/// S3 REST-API client
class client {
public:
    explicit client(const configuration& conf);
    client(const configuration& conf, const ss::abort_source& as);

    /// Stop the client
    ss::future<> shutdown();

    /// Download object from S3 bucket
    ///
    /// \param name is a bucket name
    /// \param key is an object key
    /// \return future that gets ready after request was sent
    ss::future<http::client::response_stream_ref>
    get_object(bucket_name const& name, object_key const& key);

    /// Put object to S3 bucket.
    /// \param name is a bucket name
    /// \param key is an id of the object
    /// \param payload_size is a size of the object in bytes
    /// \param body is an input_stream that can be used to read body
    /// \return future that becomes ready when the upload is completed
    ss::future<> put_object(
      bucket_name const& name,
      object_key const& key,
      size_t payload_size,
      ss::input_stream<char>&& body,
      const std::vector<object_tag>& tags = {});

    struct list_bucket_item {
        ss::sstring key;
        std::chrono::system_clock::time_point last_modified;
        size_t size_bytes;
    };
    struct list_bucket_result {
        bool is_truncated;
        ss::sstring prefix;
        std::vector<list_bucket_item> contents;
    };
    ss::future<list_bucket_result> list_objects_v2(
      const bucket_name& name,
      std::optional<object_key> prefix = std::nullopt,
      std::optional<object_key> start_after = std::nullopt,
      std::optional<size_t> max_keys = std::nullopt);

    ss::future<>
    delete_object(const bucket_name& bucket, const object_key& key);

private:
    request_creator _requestor;
    http::client _client;
};

} // namespace s3
