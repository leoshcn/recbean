"""WAV -> m4a (AAC) for meeting downloads (task 10-20-meeting-audio-m4a).

The WAV stays the source of truth (playback, resume, crash repair, ASR); the m4a next to it
is a derived cache that can be deleted at any time. It is regenerated whenever it is missing
or older than the WAV (mtime), so resumed recordings and historical ones are handled the same
way. Only this module runs ffmpeg (system binary, installed in the Docker image).
"""

from __future__ import annotations

import asyncio
import logging
import os
import shutil
from pathlib import Path

log = logging.getLogger("recbean.transcode")

# AAC-LC mono 16 kHz 32 kbps (~14 MB/h, ~1/8 of the WAV); plenty for speech recognition.
FFMPEG_ARGS = ["-vn", "-ac", "1", "-ar", "16000", "-c:a", "aac", "-b:a", "32k",
               "-movflags", "+faststart", "-f", "mp4"]
STDERR_TAIL = 500

# Per-WAV locks so concurrent requests (or a request racing the post-recording background
# job) transcode a file only once; entries are dropped when nobody holds or waits for them.
_locks: dict[str, tuple[asyncio.Lock, int]] = {}


class TranscodeError(Exception):
    """ffmpeg missing or failed; the WAV is untouched."""


def m4a_path(wav: Path) -> Path:
    return Path(wav).with_suffix(".m4a")


def is_fresh(wav: Path, m4a: Path) -> bool:
    """m4a exists and is not older than the WAV."""
    try:
        return m4a.stat().st_mtime >= Path(wav).stat().st_mtime
    except OSError:
        return False


async def _ffmpeg(wav: Path, out: Path) -> None:
    exe = shutil.which("ffmpeg")
    if not exe:
        raise TranscodeError("ffmpeg not found")
    try:
        proc = await asyncio.create_subprocess_exec(
            exe, "-nostdin", "-y", "-loglevel", "error", "-i", str(wav), *FFMPEG_ARGS, str(out),
            stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE)
    except (OSError, NotImplementedError) as e:  # e.g. no subprocess support on the loop
        raise TranscodeError(f"cannot start ffmpeg: {e!r}") from e
    try:
        _, err = await proc.communicate()
    except asyncio.CancelledError:
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        raise
    if proc.returncode != 0:
        tail = err.decode("utf-8", "replace").strip()[-STDERR_TAIL:]
        raise TranscodeError(f"ffmpeg exit {proc.returncode}: {tail}")


async def _transcode(wav: Path, m4a: Path) -> None:
    tmp = m4a.with_name(m4a.name + ".tmp")
    try:
        await _ffmpeg(wav, tmp)
        try:
            os.replace(tmp, m4a)  # atomic: a download never sees a half-written file
        except OSError as e:  # e.g. Windows: old m4a still open by a download
            raise TranscodeError(f"cannot replace {m4a.name}: {e!r}") from e
    except BaseException:
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        raise


async def ensure_m4a(wav: Path) -> Path:
    """Return the m4a for ``wav``, transcoding first if it is missing or stale.

    Raises ``TranscodeError`` if ffmpeg is unavailable or fails.
    """
    wav = Path(wav)
    key = str(wav.resolve())
    lock, users = _locks.get(key, (None, 0))
    if lock is None:
        lock = asyncio.Lock()
    _locks[key] = (lock, users + 1)
    try:
        async with lock:
            m4a = m4a_path(wav)
            if not is_fresh(wav, m4a):
                if not wav.is_file():
                    raise TranscodeError(f"{wav} not found")
                loop = asyncio.get_running_loop()
                t0 = loop.time()
                await _transcode(wav, m4a)
                log.info("transcoded %s -> %s in %.1fs", wav.name, m4a.name, loop.time() - t0)
            return m4a
    finally:
        lock, users = _locks[key]
        if users <= 1:
            del _locks[key]
        else:
            _locks[key] = (lock, users - 1)
