"""The fleet adapter's nav graph file: read with its hash, saved atomically after a version check."""

import hashlib
import os
import shutil
import tempfile
from pathlib import Path

import yaml


def read(path: Path) -> dict:
    """Content, sha256 and modification time of the file."""
    data = path.read_bytes()
    return {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'yaml': data.decode('utf-8'),
            'mtime_ms': int(path.stat().st_mtime * 1000)}


def check(text: str) -> str:
    """'' when the text is a nav graph with at least one level of vertices and lanes, else an error code."""
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError:
        return 'bad_yaml'
    levels = doc.get('levels') if isinstance(doc, dict) else None
    if not isinstance(levels, dict) or not levels:
        return 'no_levels'
    for level in levels.values():
        if not isinstance(level, dict) or not isinstance(level.get('vertices', []), list) \
                or not isinstance(level.get('lanes', []), list):
            return 'bad_level'
        count = len(level.get('vertices', []))
        for lane in level.get('lanes', []):
            if not isinstance(lane, list) or len(lane) < 2 or not all(isinstance(i, int) and 0 <= i < count for i in lane[:2]):
                return 'bad_lane'
    return ''


def save(path: Path, text: str, base_sha256: str) -> tuple[bool, str, str]:
    """Replace the file when it still has `base_sha256`; the previous content goes to <path>.bak."""
    error = check(text)
    if error:
        return False, error, ''
    current = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else ''
    if current and current != base_sha256:
        return False, 'stale', 'the nav graph changed since it was read'
    if path.exists():
        shutil.copy2(path, path.with_name(path.name + '.bak'))
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=path.name + '.', suffix='.tmp')
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        if path.exists():
            os.chmod(tmp, path.stat().st_mode & 0o777)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    return True, '', hashlib.sha256(text.encode()).hexdigest()
