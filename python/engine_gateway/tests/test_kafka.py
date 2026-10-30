"""Against a real broker: messages produced to a fresh prefix show up in the read model through KafkaSource.

Skipped unless TE_KAFKA_BOOTSTRAP names a broker (as the C++ RealBroker test) and confluent-kafka is installed:
    docker compose -f docker-compose.kafka.yml up -d
    TE_KAFKA_BOOTSTRAP=localhost:9092 python -m unittest discover -s tests
"""

import os
import time
import unittest
import uuid

from support import bar, fill, order, signal

from engine_gateway.read_model import Store
from engine_gateway.sources import KafkaSource

BOOTSTRAP = os.environ.get("TE_KAFKA_BOOTSTRAP")

try:
    import confluent_kafka
except ImportError:
    confluent_kafka = None


@unittest.skipUnless(BOOTSTRAP and confluent_kafka, "needs TE_KAFKA_BOOTSTRAP and the confluent-kafka package")
class RealBroker(unittest.TestCase):
    def test_produced_messages_reach_the_read_model(self):
        prefix = f"te.gwtest{uuid.uuid4().hex[:8]}"
        producer = confluent_kafka.Producer({"bootstrap.servers": BOOTSTRAP})
        messages = [("market_data", bar("AAPL", 30, "100")), ("signal", signal(1, "AAPL", "buy", 30)),
                    ("order", order(1, "AAPL", "buy", 10, "filled", 30, filled=10)),
                    ("fill", fill(1, 1, "AAPL", 10, "100", 30))]
        for event_type, value in messages:
            producer.produce(f"{prefix}.{event_type}", value=value, key=b"AAPL")
        self.assertEqual(producer.flush(10), 0)

        store = Store()
        source = KafkaSource(store, BOOTSTRAP, [prefix])
        source.start()
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                run = store.get(prefix)
                if run is not None and sum(run.counts.values()) == len(messages):
                    break
                time.sleep(0.2)
            run = store.get(prefix)
            self.assertIsNotNone(run, store.last_error)
            self.assertEqual(sum(run.counts.values()), len(messages))
            self.assertEqual(run.portfolio()["cash"], "99000")
            self.assertEqual(store.source_status, "connected")
        finally:
            source.stop()


if __name__ == "__main__":
    unittest.main()
