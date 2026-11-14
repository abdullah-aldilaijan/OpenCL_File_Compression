"""Disk activity of other programs whose name or command line contains OCLZIP_BENCH_OTHERS (unset: none).

python -I others.py wait   -> returns once they did < 64 MB of I/O for 60 s in a row
Imported by run.py to measure their I/O during a run.
"""
import json, os, subprocess, sys, time

QUERY = ("Get-CimInstance Win32_Process | Where-Object { $_.Name -like '*{0}*' -or "
         "($_.CommandLine -like '*{0}*' -and $_.Name -notlike 'powershell*' -and $_.Name -notlike 'grep*' -and "
         "$_.CommandLine -notlike '*shell-snapshots*' -and $_.CommandLine -notlike '*others*') } | "
         "ForEach-Object { @{pid=$_.ProcessId; name=$_.Name; io=[int64]$_.ReadTransferCount + [int64]$_.WriteTransferCount} } | "
         "ConvertTo-Json -Compress")


PATTERN = os.environ.get("OCLZIP_BENCH_OTHERS", "")


def snapshot():
    """{pid: (name, total I/O bytes so far)}"""
    if not PATTERN:
        return {}
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command", QUERY.replace("{0}", PATTERN)], capture_output=True, text=True).stdout
        data = json.loads(out) if out.strip() else []
        if isinstance(data, dict):
            data = [data]
        return {d["pid"]: (d["name"], int(d["io"])) for d in data}
    except Exception:
        return {}


class Meter:
    """Adds up I/O done by those programs between calls to sample()."""

    def __init__(self):
        self.last = snapshot()
        self.bytes = 0
        self.names = set()

    def sample(self):
        now = snapshot()
        delta = 0
        for pid, (name, io) in now.items():
            before = self.last.get(pid, (name, 0))[1]  # a new process counts from zero
            if io > before:
                delta += io - before
                if io - before > (16 << 20):
                    self.names.add(name)
        self.last = now
        self.bytes += delta
        return delta


def wait_quiet(window=60, step=5, limit=64 << 20):
    m = Meter()
    calm = 0
    while calm < window:
        time.sleep(step)
        calm = calm + step if m.sample() < limit * step / window else 0


if __name__ == "__main__" and sys.argv[1:] == ["wait"]:
    wait_quiet()
