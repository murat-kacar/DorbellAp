#!/usr/bin/env python3
"""CI check of a Burn profile build (ADR 0015): the resolved security options, the layout, and the RSA-3072 Secure
Boot v2 signatures of the merged image, signed with a throw-away key exactly as the flasher signs (shared code in
flasher/flasher_images.py).

Usage (inside the ESP-IDF environment, after `idf.py -B build_burn ... build`):
    python tools/check_burn_build.py firmware/panel
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "flasher"))

from flasher_images import (  # noqa: E402
    BURN_OVERLAY_PATH,
    generate_burn_signing_key,
    read_sdkconfig_overlay,
    sign_and_merge_burn_image,
)


def run(cmd: list[str]) -> int:
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.call(cmd)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    project_dir = (REPO_ROOT / sys.argv[1]).resolve()
    build_dir = project_dir / "build_burn"
    with tempfile.TemporaryDirectory(prefix="dorbellap_ci_sbv2_") as tmpdir:
        tmp = Path(tmpdir)
        key = tmp / "ci_sbv2.pem"
        try:
            generate_burn_signing_key(sys.executable, run, key)
            info = sign_and_merge_burn_image(
                sys.executable, run, build_dir, read_sdkconfig_overlay(BURN_OVERLAY_PATH), key, tmp / "merged_burn.bin"
            )
        except (RuntimeError, ValueError, OSError) as exc:
            print(f"::error::Burn build check failed for {project_dir.name}: {exc}")
            return 1
    print(
        f"Verified Burn build of {project_dir.name}: options, layout and RSA-3072 signatures "
        f"(bootloader {info['signed_bootloader_size']} B, app {info['signed_app_size']} B of {info['app_size']} B)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
