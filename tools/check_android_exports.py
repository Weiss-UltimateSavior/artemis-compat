#!/usr/bin/env python3
"""Check the Android compatibility product's required dynamic entry points.

This checks symbol presence, not JNI descriptors or official-jar behavior.
Keep original-shell device validation separate. Additional exports are allowed.
"""
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("library")
parser.add_argument("--nm", required=True, help="NDK llvm-nm executable")
args = parser.parse_args()
required = {
    "JNI_OnLoad",
    "ANativeActivity_onCreate",
    "Java_com_ies_1net_artemis_ArtemisActivity_ExecuteTag",
    "Java_com_ies_1net_artemis_ArtemisActivity_EmulateKeyEvent",
    "Java_com_ies_1net_artemis_ArtemisActivity_OnFinishVideo",
    "Java_com_ies_1net_artemis_ArtemisActivity_OnFinishPurchase",
    "Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery",
    "Java_moe_artemis_gui_Dialog_OnClose",
    "Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery__III",
    "Java_com_ies_1net_artemis_debug_DebugBridge_nativeInstall",
    "PauseAllInstance",
    "ResumeAllInstance",
}
output = subprocess.check_output(
    [args.nm, "--dynamic", "--defined-only", args.library], text=True
)
symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
missing = required - symbols
if missing:
    raise SystemExit("Missing dynamic exports: " + ", ".join(sorted(missing)))
print(f"PASS: {len(required)} required Android entry points exported")
