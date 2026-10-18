"""Device pairing: device requests a 4-digit code, Web confirms, device fetches token.

Pending pairings live in memory only (a server restart just means re-pairing); the
plaintext token is held only until the device fetches it. The DB stores a SHA-256 hash.
"""

from __future__ import annotations

import hashlib
import secrets
import threading
import time
from dataclasses import dataclass
from typing import Optional

from .store import Store


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


@dataclass
class Pairing:
    device_id: str
    code: str
    created_at: float
    token: Optional[str] = None  # set once confirmed, cleared after fetch

    @property
    def confirmed(self) -> bool:
        return self.token is not None


class PairingManager:
    def __init__(self, store: Store, ttl_s: float = 600) -> None:
        self.store = store
        self.ttl_s = ttl_s
        self._lock = threading.Lock()
        self._by_device: dict[str, Pairing] = {}

    def _expire(self) -> None:
        now = time.time()
        for dev, p in list(self._by_device.items()):
            if now - p.created_at > self.ttl_s:
                del self._by_device[dev]

    def request(self, device_id: str) -> Pairing:
        with self._lock:
            self._expire()
            p = self._by_device.get(device_id)
            if p and not p.confirmed:
                return p
            used = {x.code for x in self._by_device.values()}
            while True:
                code = f"{secrets.randbelow(10000):04d}"
                if code not in used:
                    break
            p = Pairing(device_id, code, time.time())
            self._by_device[device_id] = p
            return p

    def pending(self) -> list[Pairing]:
        with self._lock:
            self._expire()
            return [p for p in self._by_device.values() if not p.confirmed]

    def confirm(self, code: str, name: Optional[str] = None) -> Optional[Pairing]:
        with self._lock:
            self._expire()
            for p in self._by_device.values():
                if p.code == code and not p.confirmed:
                    token = secrets.token_urlsafe(32)
                    self.store.upsert_device(p.device_id, hash_token(token), name)
                    p.token = token
                    return p
            return None

    def poll(self, device_id: str) -> Optional[Pairing]:
        """Return the pairing; if confirmed, the token is handed out once and removed."""
        with self._lock:
            self._expire()
            p = self._by_device.get(device_id)
            if p and p.confirmed:
                del self._by_device[device_id]
            return p
