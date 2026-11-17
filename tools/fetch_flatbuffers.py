"""Build the protocol-pinned flatc and headers under deps/flatbuffers (Linux)."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

VERSION = "25.12.19"
SHA256 = "f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="use a previously downloaded source tar.gz")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    destination = Path(__file__).resolve().parents[1] / "deps/flatbuffers"
    # TMPDIR can point to a larger volume without placing build artifacts in the repository.
    with tempfile.TemporaryDirectory(prefix="poke-flatbuffers-") as temporary:
        work = Path(temporary)
        if args.archive:
            payload = args.archive.read_bytes()
        else:
            with urlopen(f"https://codeload.github.com/google/flatbuffers/tar.gz/refs/tags/v{VERSION}", timeout=60) as response:
                payload = response.read()
        if hashlib.sha256(payload).hexdigest() != SHA256:
            raise RuntimeError("FlatBuffers archive SHA-256 mismatch")
        archive = work / "source.tar.gz"
        archive.write_bytes(payload)
        with tarfile.open(archive) as package:
            members = []
            for member in package.getmembers():
                target = (work / member.name).resolve()
                if not target.is_relative_to(work.resolve()):
                    raise RuntimeError("unsafe source archive entry")
                # Upstream Java/TypeScript test fixtures contain links; flatc does not need them.
                if member.issym() or member.islnk():
                    continue
                if not (member.isfile() or member.isdir()):
                    raise RuntimeError("unsupported source archive entry")
                members.append(member)
            package.extractall(work, members=members)
        source = work / f"flatbuffers-{VERSION}"
        build = work / "build"
        subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                        "-DFLATBUFFERS_BUILD_TESTS=OFF", "-DFLATBUFFERS_BUILD_FLATLIB=OFF",
                        "-DFLATBUFFERS_BUILD_FLATHASH=OFF", "-DFLATBUFFERS_BUILD_SHAREDLIB=OFF"], check=True)
        subprocess.run(["cmake", "--build", str(build), "--target", "flatc", "--parallel", str(args.jobs)], check=True)
        (destination / "bin").mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / "flatc", destination / "bin/flatc")
        shutil.copytree(source / "include/flatbuffers", destination / "include/flatbuffers", dirs_exist_ok=True)
    print(f"FlatBuffers {VERSION}: {destination}")


if __name__ == "__main__":
    main()
