#!/usr/bin/env python3
"""Builds "VoiceInk Button Toggle.shortcut": one action, VoiceInk's "Toggle VoiceInk Recorder" intent.

    python3 mac/make_toggle_shortcut.py && open "build/VoiceInk Button Toggle.shortcut"

Unlike the ⌥/ hotkey, running this shortcut is not blocked by secure input (a focused password
field). See docs/PRD-mic-presence.md, section 9.
"""
import os
import plistlib
import subprocess
import sys

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "build"
NAME = "VoiceInk Button Toggle"

intent = {
    "AppIntentIdentifier": "ToggleMiniRecorderIntent",
    "BundleIdentifier": "com.prakashjoshipax.VoiceInk",
    "Name": "VoiceInk",
    "TeamIdentifier": "V6J6A3VWY2",
}
workflow = {
    "WFWorkflowMinimumClientVersion": 900,
    "WFWorkflowMinimumClientVersionString": "900",
    "WFWorkflowClientVersion": "2607.0.2",
    "WFWorkflowIcon": {"WFWorkflowIconStartColor": 4282601983, "WFWorkflowIconGlyphNumber": 61440},
    "WFWorkflowTypes": [],
    "WFWorkflowInputContentItemClasses": [],
    "WFWorkflowImportQuestions": [],
    "WFWorkflowActions": [{
        "WFWorkflowActionIdentifier": "com.prakashjoshipax.VoiceInk.ToggleMiniRecorderIntent",
        "WFWorkflowActionParameters": {"AppIntentDescriptor": intent},
    }],
}

os.makedirs(OUT_DIR, exist_ok=True)
unsigned = os.path.join(OUT_DIR, NAME + ".unsigned.shortcut")
signed = os.path.join(OUT_DIR, NAME + ".shortcut")
with open(unsigned, "wb") as f:
    plistlib.dump(workflow, f, fmt=plistlib.FMT_BINARY)
# macOS only imports signed shortcuts
subprocess.run(["shortcuts", "sign", "--mode", "anyone", "--input", unsigned, "--output", signed], check=True)
os.remove(unsigned)
print(signed)
