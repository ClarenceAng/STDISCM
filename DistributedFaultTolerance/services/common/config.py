"""Settings read from environment variables, which docker-compose.yml sets for each node."""

import os


class ConfigError(Exception):
    """A setting is missing or out of range. The node reports it and exits instead of guessing."""


def text(name: str, default: str | None = None) -> str:
    value = os.environ.get(name)
    if value is None or value.strip() == "":
        if default is None:
            raise ConfigError(f"environment variable {name} is not set")
        return default
    return value.strip()


def integer(name: str, default: int, low: int, high: int) -> int:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = int(raw)
    except ValueError:
        raise ConfigError(f"{name}={raw!r} is not a whole number") from None
    if not low <= value <= high:
        raise ConfigError(f"{name}={value} is outside {low}..{high}")
    return value


def seconds(name: str, default: float, low: float, high: float) -> float:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        value = float(raw)
    except ValueError:
        raise ConfigError(f"{name}={raw!r} is not a number of seconds") from None
    if not low <= value <= high:
        raise ConfigError(f"{name}={value} is outside {low}..{high} seconds")
    return value


def host_list(name: str, default: str) -> list[str]:
    hosts = [h.strip() for h in text(name, default).split(",")]
    if any(h == "" for h in hosts):
        raise ConfigError(f"{name} has an empty host name in it")
    return hosts


def read_file(name: str, path: str, made_by: str = "the setup container") -> bytes:
    """Reads a file named by a setting, saying which setting and why when it can't. `made_by` is
    what makes the file, for the messages when it isn't there."""
    if os.path.isdir(path):
        # Docker Desktop mounts a missing file as an empty folder it makes in its place
        raise ConfigError(f"{name}: {path} is a folder, not a file. Docker makes an empty folder in place of a "
                          f"file it was told to mount that didn't exist yet. The real file is made by {made_by}.")
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        raise ConfigError(f"{name}: {path} does not exist (it is made by {made_by})") from None
    except PermissionError:
        raise ConfigError(f"{name}: no permission to read {path}") from None
    except OSError as exc:
        raise ConfigError(f"{name}: could not read {path}: {exc.strerror}") from None
