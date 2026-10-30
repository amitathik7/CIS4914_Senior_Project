"""Start the engine gateway: the Engine console's API (docs/adr/0006-web-frontend-api-contract.md), read from the
engine's Kafka topics (docs/EVENT_BUS.md).

    python serve.py [--brokers localhost:9092] [--topic-prefix NAME] [--port 8080]
    python serve.py --replay FILE          # a recording, no broker needed

Kafka needs `pip install -r requirements.txt` (confluent-kafka); --replay needs only the standard library.
See docs/ENGINE_GATEWAY.md.
"""

import sys

from engine_gateway.cli import main

if __name__ == "__main__":
    sys.exit(main())
