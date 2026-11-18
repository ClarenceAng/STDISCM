import logging
import sys


def setup(node: str) -> None:
    """Log lines go to stdout, so `docker compose logs <node>` shows them."""
    logging.basicConfig(
        level=logging.INFO,
        stream=sys.stdout,
        format=f"%(asctime)s %(levelname)-7s [{node}] %(name)s: %(message)s",
    )
