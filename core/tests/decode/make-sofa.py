# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the two SOFA fixtures beside this script, made rather than measured, so the reader has a truth:

  made.sofa          8 directions, 2 receivers, 64 samples at 48 kHz, the left ear first, datasets compressed
  made-swapped.sofa  the same with the receivers the other way round in the file, which the reader must refuse

Sample n of measurement m at ear e (0 left, 1 right) is (m + 1)(e + 1) / 1000 + n / 100000, a formula the test
recomputes. Written as NetCDF-4, which is what AES69 says a SOFA file is and what libmysofa's reader expects.
Needs netCDF4:  uv run --with netCDF4 --with numpy core/tests/decode/make-sofa.py
"""
from pathlib import Path

import netCDF4
import numpy as np

HERE = Path(__file__).parent
DIRECTIONS = [(0, 0), (90, 0), (-90, 0), (180, 0), (0, 90), (0, -90), (45, 30), (-135, -20)]  # azimuth, elevation
M, R, N, FS = len(DIRECTIONS), 2, 64, 48000.0


def write(path: Path, left_first: bool):
    f = netCDF4.Dataset(path, "w", format="NETCDF4")
    for k, v in {
        "Conventions": "SOFA", "Version": "2.1", "SOFAConventions": "SimpleFreeFieldHRIR",
        "SOFAConventionsVersion": "1.0", "DataType": "FIR", "RoomType": "free field",
        "Title": "a made head for bambi's tests", "DateCreated": "2026-10-10 00:00:00",
        "DateModified": "2026-10-10 00:00:00", "APIName": "netCDF4-python", "APIVersion": netCDF4.__version__,
        "AuthorContact": "bambi", "Organization": "bambi", "License": "GPL-3.0-or-later",
        "ListenerShortName": "made", "DatabaseName": "bambi tests",
    }.items():
        f.setncattr(k, v)
    for name, size in {"M": M, "R": R, "E": 1, "N": N, "I": 1, "C": 3}.items():
        f.createDimension(name, size)

    def var(name, dims, data, **attrs):
        v = f.createVariable(name, "f8", dims, zlib=True)
        v[:] = np.asarray(data, dtype=np.float64)
        for k, a in attrs.items():
            v.setncattr(k, a)

    var("ListenerPosition", ("I", "C"), [[0.0, 0.0, 0.0]], Type="cartesian", Units="metre")
    var("ListenerUp", ("I", "C"), [[0.0, 0.0, 1.0]], Type="cartesian", Units="metre")
    var("ListenerView", ("I", "C"), [[1.0, 0.0, 0.0]], Type="cartesian", Units="metre")
    ears = [[0.0, 0.09, 0.0], [0.0, -0.09, 0.0]] if left_first else [[0.0, -0.09, 0.0], [0.0, 0.09, 0.0]]
    var("ReceiverPosition", ("R", "C", "I"), np.array(ears).reshape(R, 3, 1), Type="cartesian", Units="metre")
    var("SourcePosition", ("M", "C"), [[az, el, 1.0] for az, el in DIRECTIONS], Type="spherical",
        Units="degree, degree, metre")
    var("EmitterPosition", ("E", "C", "I"), [[[0.0], [0.0], [0.0]]], Type="cartesian", Units="metre")
    var("Data.SamplingRate", ("I",), [FS], Units="hertz")
    var("Data.Delay", ("I", "R"), np.zeros((1, R)))
    ir = np.zeros((M, R, N))
    for m in range(M):
        for r in range(R):
            ear = r if left_first else 1 - r
            ir[m, r, :] = (m + 1) * (ear + 1) / 1000 + np.arange(N) / 100000
    var("Data.IR", ("M", "R", "N"), ir)
    f.close()


write(HERE / "made.sofa", True)
write(HERE / "made-swapped.sofa", False)
print("wrote", HERE / "made.sofa", HERE / "made-swapped.sofa")
