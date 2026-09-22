"""Files the dashboard keeps: where they live, atomic writes and a debounced background writer."""

import os
import sys
import tempfile
import threading
import time
from pathlib import Path

CONFIG_DIR_ENV = "EIU_CONFIG_DIR"
_APP_DIR_NAME = "eiu_fleet_ui"


def user_config_dir() -> Path:
    """Folder for the dashboard's own state: EIU_CONFIG_DIR, else the XDG config folder."""
    explicit = os.environ.get(CONFIG_DIR_ENV)
    if explicit:
        return Path(explicit)
    base = os.environ.get("XDG_CONFIG_HOME")
    return (Path(base) if base else Path.home() / ".config") / _APP_DIR_NAME


def _read_umask() -> int:
    mask = os.umask(0)
    os.umask(mask)
    return mask


_UMASK = _read_umask()


def write_atomic(path, text: str) -> None:
    """Write text through a temporary file so a crash cannot leave the target half written."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # A replaced file keeps its mode and owner; a new one gets the umask's mode and its folder's owner.
    replaced = path.exists()
    template = path.stat() if replaced else path.parent.stat()
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        os.chmod(temporary, template.st_mode & 0o777 if replaced else 0o666 & ~_UMASK)
        try:
            os.chown(temporary, template.st_uid, template.st_gid)
        except OSError:
            pass   # Only a privileged process may hand a file to another owner
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


class DebouncedWriter:
    """Writes the latest submitted text of one file from a worker thread, at most once per delay."""

    def __init__(self, path, delay_sec: float, name: str = "file-writer"):
        self._path = Path(path)
        self._delay = delay_sec
        self._pending = None
        self._closed = False
        self._cond = threading.Condition()
        self._write_lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True, name=name)
        self._thread.start()

    def submit(self, text: str) -> None:
        """Queue text as the file's next content; earlier unwritten text is dropped."""
        with self._cond:
            self._pending = text
            self._cond.notify()

    def flush(self) -> None:
        """Write any queued text now."""
        self._write_pending()

    def close(self) -> None:
        """Write any queued text and stop the worker."""
        with self._cond:
            self._closed = True
            self._cond.notify_all()
        self.flush()

    def _write_pending(self) -> None:
        with self._write_lock:
            with self._cond:
                text, self._pending = self._pending, None
            if text is None:
                return
            try:
                write_atomic(self._path, text)
            except OSError as exc:
                print(f"[FILE] cannot write {self._path}: {exc}", file=sys.stderr)

    def _run(self) -> None:
        while True:
            with self._cond:
                while self._pending is None and not self._closed:
                    self._cond.wait()
                if self._closed:
                    return
                # Let changes that follow at once coalesce into one write.
                deadline = time.monotonic() + self._delay
                while not self._closed and deadline > time.monotonic():
                    self._cond.wait(deadline - time.monotonic())
            self._write_pending()
