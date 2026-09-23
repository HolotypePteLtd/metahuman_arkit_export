"""Standalone entry point for mh-arkit-export."""
import json
from pathlib import Path
import sys


def build_info():
    if getattr(sys, "frozen", False):
        return json.loads((Path(sys._MEIPASS) / "BUILD_INFO.json").read_text())
    root = Path(__file__).resolve().parents[1]
    return {"schema": 1, "application": "mh-arkit-export",
            "version": (root / "VERSION").read_text().strip(),
            "openriglogic": json.loads((root / "runtime.json").read_text())}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv == ["--version"]:
        print("mh-arkit-export " + build_info()["version"])
        return 0
    if argv == ["--build-info"]:
        print(json.dumps(build_info(), sort_keys=True))
        return 0
    try:
        if argv == ["--self-test"]:
            from smoke_fixture import self_test
            result = self_test()
            result["version"] = build_info()["version"]
            print(json.dumps(result, sort_keys=True))
        else:
            from bake_arkit_dna import main as bake
            bake(argv, standalone=True)
        return 0
    except (OSError, ValueError, ImportError, RuntimeError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
