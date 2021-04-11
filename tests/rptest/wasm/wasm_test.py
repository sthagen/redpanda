# Copyright 2021 Vectorized, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

import os
import uuid

from kafka import TopicPartition

from rptest.wasm.native_kafka_consumer import NativeKafkaConsumer
from rptest.wasm.native_kafka_producer import NativeKafkaProducer

from rptest.wasm.topic import construct_materialized_topic
from rptest.wasm.wasm_build_tool import WasmBuildTool

from rptest.tests.redpanda_test import RedpandaTest

from rptest.clients.types import TopicSpec
from rptest.clients.rpk import RpkTool

from ducktape.utils.util import wait_until

from functools import reduce


def flat_map(fn, ll):
    return reduce(lambda acc, x: acc + fn(x), ll, [])


class WasmScript:
    def __init__(self, inputs=[], outputs=[], script=None):
        self.inputs = inputs
        self.outputs = outputs
        self.script = script
        self.dir_name = str(uuid.uuid4())

    def get_artifact(self, build_dir):
        artifact = os.path.join(build_dir, self.dir_name, "dist",
                                f"{self.dir_name}.js")
        if not os.path.exists(artifact):
            raise Exception(f"Artifact {artifact} was not built")
        return artifact


class WasmTest(RedpandaTest):
    def __init__(self, test_context, extra_rp_conf=dict(), num_brokers=3):
        def enable_wasm_options():
            return dict(
                developer_mode=True,
                enable_coproc=True,
            )

        wasm_opts = enable_wasm_options()
        wasm_opts.update(extra_rp_conf)
        super(WasmTest, self).__init__(test_context,
                                       extra_rp_conf=wasm_opts,
                                       num_brokers=num_brokers)
        self._rpk_tool = RpkTool(self.redpanda)
        self._build_tool = WasmBuildTool(self._rpk_tool)

    def _build_script(self, script):
        # Build the script itself
        self._build_tool.build_test_artifacts(script)

        # Deploy coprocessor
        self._rpk_tool.wasm_deploy(
            script.get_artifact(self._build_tool.work_dir), "ducktape")

    def _start(self, topic_spec, scripts, xfactor):
        def to_output_topic_spec(output_topics):
            """
            Create a list of TopicPartitions for the set of output topics.
            Must parse the materialzied topic for the input topic to determine
            the number of partitions.
            """
            result = []
            for src, _, _ in topic_spec:
                materialized_topics = [
                    TopicSpec(name=construct_materialized_topic(
                        src.name, dest),
                              partition_count=src.partition_count,
                              replication_factor=src.replication_factor,
                              cleanup_policy=src.cleanup_policy)
                    for dest in output_topics
                ]
                result += materialized_topics
            return result

        def expand_topic_spec(etc):
            """
            Convers a TopicSpec iterable to a TopicPartitions list
            """
            return flat_map(
                lambda spec: [
                    TopicPartition(spec.name, x)
                    for x in range(0, spec.partition_count)
                ], etc)

        for script in scripts:
            self._build_script(script)

        # Calcualte expected records on all inputs / outputs
        total_inputs = reduce(lambda acc, x: acc + x[1], topic_spec, 0)

        def accrue(num_records):
            return reduce(
                lambda acc, script: acc +
                (num_records * len(script.outputs) * xfactor), scripts, 0)

        total_outputs = reduce(lambda acc, x: acc + accrue(x[1]), topic_spec,
                               0)

        input_tps = expand_topic_spec([x[0] for x in topic_spec])
        output_tps = expand_topic_spec(
            to_output_topic_spec(
                flat_map(lambda script: script.outputs, scripts)))

        self.logger.info(f"Input consumer assigned: {input_tps}")
        self.logger.info(f"Output consumer assigned: {output_tps}")

        producers = []
        for tp_spec, num_records, record_size in topic_spec:
            try:
                producer = NativeKafkaProducer(self.redpanda.brokers(),
                                               tp_spec.name, num_records,
                                               record_size)
                producer.start()
                producers.append(producer)
            except Exception as e:
                self.logger.error(f"Failed to create NativeKafkaProducer: {e}")
                raise

        input_consumer = None
        output_consumer = None
        try:
            input_consumer = NativeKafkaConsumer(self.redpanda.brokers(),
                                                 input_tps, total_inputs)
            output_consumer = NativeKafkaConsumer(self.redpanda.brokers(),
                                                  output_tps, total_outputs)
        except Exception as e:
            self.logger.error(f"Failed to create NativeKafkaConsumer: {e}")
            raise

        self.logger.info(
            f"Waiting for {total_inputs} input records and {total_outputs}"
            " result records")
        input_consumer.start()
        output_consumer.start()

        def all_done():
            # Uncomment to periodically see the amt of data read
            # self.logger.info("Input: %d" %
            #                  input_consumer.results.num_records())
            # self.logger.info("Output: %d" %
            #                  output_consumer.results.num_records())
            return input_consumer.is_finished() and \
                output_consumer.is_finished()

        timeout, backoff = self.wasm_test_timeout()
        wait_until(all_done, timeout_sec=timeout, backoff_sec=backoff)
        try:
            input_consumer.join()
            output_consumer.join()
            [x.join() for x in producers]
        except Exception as e:
            self.logger.error("Exception occured in background thread: {e}")
            raise e

        self.logger.info(
            f"Consumed {input_consumer.results.num_records()} input"
            f" records/expected {total_inputs} and"
            f" {output_consumer.results.num_records()} result records/expected"
            f" {total_outputs}")
        return (input_consumer.results, output_consumer.results)

    def wasm_test_timeout(self):
        """
        2-tuple representing timeout(0) and backoff interval(1)
        """
        return (180, 1)
