"""The request body of one gateway request: read within a total time, and never left unread when the answer goes out.

Answering and then closing with unread request bytes makes some TCP stacks reset the connection, and the client then never sees the
status line (seen on Windows as WinError 10053). So the gateway discards what it will not use before it answers. Both reads are
bounded in size AND in total time: the time allowed is a deadline for the whole body, not per read, so a client that trickles one
byte at a time cannot hold a connection (and its thread) open.
"""

from __future__ import annotations

import time
from typing import Any

CHUNK_BYTES = 65536


class RequestBody:
    """What is left of one request's body on its connection. `unread` counts the bytes not yet read."""

    def __init__(self, connection: Any, rfile: Any, declared: int) -> None:
        self.connection = connection
        self.rfile = rfile
        self.unread = max(declared, 0)

    def receive(self, length: int, seconds: float) -> bytes:
        """Exactly `length` bytes within `seconds` IN TOTAL. Raises OSError (a timeout, or the client closing early) otherwise."""
        deadline = time.monotonic() + seconds
        parts: list[bytes] = []
        try:
            while length > 0:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError("the request body did not arrive in time")
                self.connection.settimeout(left)
                chunk = self.rfile.read1(min(length, CHUNK_BYTES))
                if not chunk:
                    raise ConnectionError("the client closed the connection before the body was complete")
                parts.append(chunk)
                length -= len(chunk)
                self.unread -= len(chunk)
        finally:
            self.connection.settimeout(None)                     # the answer itself may be large and the client slow
        return b"".join(parts)

    def abandon(self) -> None:
        """The connection is no use for reading any more (it timed out or was closed): nothing is left to discard."""
        self.unread = 0

    def drain(self, limit: int, seconds: float) -> None:
        """Read and discard whatever is still unread: at most `limit` bytes, within `seconds` in total."""
        wanted = min(self.unread, limit)
        if wanted <= 0:
            return
        try:
            self.receive(wanted, seconds)
        except OSError:
            self.abandon()
