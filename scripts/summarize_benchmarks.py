"""Summarize independent benchmark processes; Python standard library only."""
import csv
from pathlib import Path
import statistics
import sys

directory = Path(sys.argv[1])
groups = {}
for path in sorted(directory.glob("run-*.csv")):
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            # Treat recovery's three samples as observations at the same WAL size.
            name = row["workload"].split("_sample")[0]
            groups.setdefault(name, []).append(row)
if not groups:
    raise SystemExit("No benchmark CSV results found")
print("| Workload | Samples | Median ms | Min–max ms | Median ops/s | WAL bytes | Heap hit rate | Index hit rate |")
print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
for name, rows in groups.items():
    durations = [1000 * float(row["seconds"]) for row in rows]
    rates = [float(row["ops_per_second"]) for row in rows]
    heap = statistics.median(float(row["heap_hit_rate"]) for row in rows)
    index = statistics.median(float(row["index_hit_rate"]) for row in rows)
    sizes = {int(row["wal_bytes"]) for row in rows}
    if len(sizes) != 1:
        raise SystemExit(f"Inconsistent WAL sizes in {name}: {sizes}")
    print(f"| {name} | {len(rows)} | {statistics.median(durations):.3f} | "
          f"{min(durations):.3f}–{max(durations):.3f} | {statistics.median(rates):.1f} | "
          f"{sizes.pop()} | {heap:.3%} | {index:.3%} |")
