#!/usr/bin/env python3
"""Check a native BinaryStreamWriter round-trip without changing the source DNA.

Run with the matching Python and bindings on PYTHONPATH. This deliberately
uses the default reader configuration to preserve every LOD and source axes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile


def roundtrip(path):
    import dna

    stream = dna.FileStream(str(path), dna.AccessMode_Read, dna.OpenMode_Binary)
    reader = dna.BinaryStreamReader(stream)
    reader.read()
    if not dna.Status.isOk():
        raise ValueError(dna.Status.get().message)
    with tempfile.TemporaryDirectory(prefix="DNA roundtrip ") as directory:
        output = Path(directory) / "roundtrip.dna"
        destination = dna.FileStream(str(output), dna.AccessMode_Write, dna.OpenMode_Binary)
        writer = dna.BinaryStreamWriter(destination)
        writer.setFrom(reader)
        writer.write()
        if not dna.Status.isOk():
            raise ValueError(dna.Status.get().message)
        original, written = path.read_bytes(), output.read_bytes()
    return {"fidelity": original == written, "source_sha256": hashlib.sha256(original).hexdigest(),
            "roundtrip_sha256": hashlib.sha256(written).hexdigest(),
            "lods": reader.getLODCount(), "meshes": reader.getMeshCount(), "joints": reader.getJointCount()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dna", type=Path)
    result = roundtrip(parser.parse_args().dna)
    print("VERDICT " + json.dumps(result, sort_keys=True))
    raise SystemExit(0 if result["fidelity"] else 1)
