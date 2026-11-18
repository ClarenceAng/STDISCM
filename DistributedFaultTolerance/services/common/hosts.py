"""Looks up the database nodes' addresses in the background.

When a container stops, Docker's DNS takes about 4 s to answer that its name no longer exists.
If every new connection looked the names up itself, every one would wait those 4 s while a
database node is down, longer than a page should take. Instead, a lookup of each database node's
name starts every second, and connections use the newest address found (libpq's hostaddr),
skipping a node whose name doesn't resolve without waiting at all.

Lookups overlap: a new one starts every second even while a slow one is still waiting, and only
the newest answer counts. So a node that comes back is seen within about a second, not after the
4 s lookup that started while it was gone.
"""

import logging
import socket
import threading
import time

log = logging.getLogger("hosts")


class HostResolver:
    def __init__(self, hosts: list[str], port: int, interval: float = 1.0, first_wait: float = 5.0):
        self._hosts = list(hosts)
        self._port = port
        self._interval = interval
        self._lock = threading.Lock()
        self._address: dict[str, str | None] = {h: None for h in hosts}
        self._problem: dict[str, str] = {h: "not looked up yet" for h in hosts}
        self._applied: dict[str, int] = {h: 0 for h in hosts}   # newest lookup whose answer is in use
        first_round = []
        for host in self._hosts:
            done = threading.Event()
            first_round.append(done)
            threading.Thread(target=self._loop, args=(host, done), name=f"resolve-{host}", daemon=True).start()
        deadline = time.monotonic() + first_wait
        for done in first_round:
            done.wait(max(0.0, deadline - time.monotonic()))

    def _loop(self, host: str, first_done: threading.Event) -> None:
        number = 0
        while True:
            number += 1
            threading.Thread(target=self._resolve, args=(host, number, first_done),
                             name=f"lookup-{host}", daemon=True).start()
            time.sleep(self._interval)

    def _resolve(self, host: str, number: int, first_done: threading.Event) -> None:
        try:
            infos = socket.getaddrinfo(host, self._port, type=socket.SOCK_STREAM)
            address, problem = infos[0][4][0], ""
        except socket.gaierror as exc:
            address, problem = None, f"its name doesn't resolve ({exc.strerror})"
        except OSError as exc:
            address, problem = None, f"looking its name up failed ({exc.strerror})"
        with self._lock:
            if number < self._applied[host]:
                return   # a newer lookup has answered already; this slower one is out of date
            self._applied[host] = number
            before = self._address[host]
            self._address[host] = address
            self._problem[host] = problem
        first_done.set()
        if address != before:
            if address:
                log.info("%s is at %s", host, address)
            else:
                log.warning("%s has no address any more: %s", host, problem)

    def usable(self, order: list[str]) -> list[tuple[str, str]]:
        """(name, address) for each node in `order` whose name resolves right now."""
        with self._lock:
            return [(h, self._address[h]) for h in order if self._address.get(h)]

    def address(self, host: str) -> tuple[str | None, str]:
        with self._lock:
            return self._address.get(host), self._problem.get(host, "unknown host")

    def problems(self) -> str:
        with self._lock:
            return "; ".join(f"{h}: {self._problem[h]}" for h in self._hosts if not self._address[h])
