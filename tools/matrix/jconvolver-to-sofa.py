# SPDX-License-Identifier: GPL-3.0-or-later
"""A jconvolver decoder set -- a configuration and its WAV files -- as one SOFA file the decoder reads.

  uv run --with netCDF4 --with numpy tools/matrix/jconvolver-to-sofa.py <config.conf> <out.sofa>
         [--taps N] [--normalization N3D|SN3D] [--title "..."]

The configuration's /convolver/new gives the inputs and outputs, each /impulse/read one filter from a channel of a
WAV file, with its gain and delay. The filters become a SOFA 2.1 GeneralFIR-E file: one measurement, the inputs as
emitters, the outputs as receivers, cut to --taps with a short fade (the IKO's 4096-tap filters hold 99.99 % of
their energy in the first 1024). The spherical-harmonic order, channel order and normalisation the filters expect
on their input go in as global attributes, since the configuration does not say; --normalization is what the
maker says, N3D unless told.
"""
import argparse
import datetime
import pathlib
import struct

import netCDF4
import numpy as np


def read_wav(path: pathlib.Path):
    """A float32 or int16/24 WAV as (channels, frames) float64 and its rate; RIFF chunks walked by hand."""
    b = path.read_bytes()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", f"{path}: not a WAV file"
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        tag, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            data = body
        pos += 8 + size + (size & 1)
    assert fmt and data is not None, f"{path}: no fmt or data chunk"
    kind, channels, rate, _, _, bits = fmt
    if kind == 3 and bits == 32:
        x = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif kind == 1 and bits == 16:
        x = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
    elif kind == 1 and bits == 24:
        raw = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3)
        x = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int8).astype(np.int32) << 16)).astype(np.float64) / 8388608.0
    else:
        raise SystemExit(f"{path}: unsupported WAV format {kind}/{bits}")
    return x.reshape(-1, channels).T, rate


def parse_conf(path: pathlib.Path):
    folder, inputs, outputs, reads = path.parent, 0, 0, []
    for line in path.read_text().splitlines():
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if words[0] == "/cd":
            folder = (path.parent / words[1]).resolve()
        elif words[0] == "/convolver/new":
            inputs, outputs = int(words[1]), int(words[2])
        elif words[0] == "/impulse/read":
            i, o, gain, delay, offset, length, chan = (int(words[1]), int(words[2]), float(words[3]), int(words[4]),
                                                       int(words[5]), int(words[6]), int(words[7]))
            reads.append((i, o, gain, delay, offset, length, chan, folder / words[8]))
    assert inputs and outputs and reads, f"{path}: no /convolver/new or no /impulse/read lines"
    return inputs, outputs, reads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("conf", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--taps", type=int, default=1024)
    ap.add_argument("--fade", type=int, default=64, help="a half-Hann over the last taps")
    ap.add_argument("--order", type=int, default=None, help="the input's spherical-harmonic order; from the inputs when omitted")
    ap.add_argument("--normalization", choices=["N3D", "SN3D"], default="N3D")
    ap.add_argument("--title", default=None)
    a = ap.parse_args()

    inputs, outputs, reads = parse_conf(a.conf)
    order = a.order if a.order is not None else int(round(inputs ** 0.5)) - 1
    assert (order + 1) ** 2 == inputs, f"{inputs} inputs is not a whole order"
    cache, rate = {}, None
    matrix = np.zeros((outputs, a.taps, inputs))  # R x N x E, the SOFA order
    kept, total = 0.0, 0.0
    for i, o, gain, delay, offset, length, chan, file in reads:
        if file not in cache:
            cache[file] = read_wav(file)
        x, r = cache[file]
        assert rate in (None, r), "the files differ in sampling rate"
        rate = r
        ir = x[chan - 1][offset:offset + length if length > 0 else None]
        ir = np.concatenate([np.zeros(delay), ir]) * gain
        total += float(np.sum(ir ** 2))
        cut = ir[:a.taps].copy()
        if a.fade > 0 and cut.size >= a.fade:
            cut[-a.fade:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(a.fade) / a.fade)
        kept += float(np.sum(cut ** 2))
        matrix[o - 1, :cut.size, i - 1] = cut

    f = netCDF4.Dataset(a.out, "w", format="NETCDF4")
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    for k, v in {
        "Conventions": "SOFA", "Version": "2.1", "SOFAConventions": "GeneralFIR-E", "SOFAConventionsVersion": "1.0",
        "DataType": "FIR-E", "RoomType": "free field", "Title": a.title or a.conf.stem,
        "DateCreated": now, "DateModified": now, "APIName": "netCDF4-python", "APIVersion": netCDF4.__version__,
        "AuthorContact": "", "Organization": "", "License": "as the configuration's maker gives it",
        "Comment": f"a decoder filter matrix from the jconvolver configuration {a.conf.name}, cut to {a.taps} taps",
        "AmbisonicsOrder": str(order), "AmbisonicsChannelOrder": "ACN", "AmbisonicsNormalization": a.normalization,
    }.items():
        f.setncattr(k, v)
    for name, size in {"M": 1, "R": outputs, "E": inputs, "N": a.taps, "I": 1, "C": 3}.items():
        f.createDimension(name, size)

    def var(name, dims, data, **attrs):
        v = f.createVariable(name, "f8", dims, zlib=True)
        v[:] = np.asarray(data, dtype=np.float64)
        for k, at in attrs.items():
            v.setncattr(k, at)

    var("ListenerPosition", ("I", "C"), [[0.0, 0.0, 0.0]], Type="cartesian", Units="metre")
    var("ListenerUp", ("I", "C"), [[0.0, 0.0, 1.0]], Type="cartesian", Units="metre")
    var("ListenerView", ("I", "C"), [[1.0, 0.0, 0.0]], Type="cartesian", Units="metre")
    var("ReceiverPosition", ("R", "C", "I"), np.zeros((outputs, 3, 1)), Type="cartesian", Units="metre")
    var("SourcePosition", ("I", "C"), [[0.0, 0.0, 0.0]], Type="cartesian", Units="metre")
    var("EmitterPosition", ("E", "C", "I"), np.zeros((inputs, 3, 1)), Type="cartesian", Units="metre")
    var("Data.SamplingRate", ("I",), [float(rate)], Units="hertz")
    var("Data.Delay", ("I", "R", "E"), np.zeros((1, outputs, inputs)))
    var("Data.IR", ("M", "R", "N", "E"), matrix[np.newaxis, :, :, :])
    f.close()
    print(f"{a.out}: {inputs} in (order {order}, {a.normalization}), {outputs} out, {a.taps} taps at {rate} Hz, "
          f"{100 * kept / total:.3f} % of the filters' energy kept, {a.out.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
