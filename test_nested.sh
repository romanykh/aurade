#!/bin/bash
# Script to launch AuraDE nested inside your current Wayland/X11 session
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export WLR_BACKENDS=wayland

# A nested AuraDE shares the physical touchpad with the host compositor, so its
# own gesture engine stays off (detected from WLR_BACKENDS=wayland). Set
# AURADE_ENABLE_GESTURES=1 to force it on for gesture testing in the nest.
echo "Starting AuraDE nested compositor (gestures handled by the host)..."
"$SCRIPT_DIR/aurade"
