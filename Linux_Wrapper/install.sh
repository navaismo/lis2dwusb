#!/usr/bin/env bash
set -euo pipefail

if [[ $(id -u) -ne 0 ]]; then
    echo "Run as root with OCTOPRINT_USER set to the service account." >&2
    exit 1
fi
service_user=${OCTOPRINT_USER:-${SUDO_USER:-}}
if [[ ! $service_user =~ ^[a-z_][a-z0-9_-]*[$]?$ ]] || ! id "$service_user" >/dev/null 2>&1 || [[ $(id -u "$service_user") -eq 0 ]]; then
    echo "Set OCTOPRINT_USER to an existing, non-root OctoPrint service account." >&2
    exit 1
fi
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
getent group dialout >/dev/null || { echo "The dialout serial group is required." >&2; exit 1; }
apt-get update
apt-get install -y build-essential make
make -C "$script_dir"
make -C "$script_dir" install
usermod -a -G dialout "$service_user"
echo "Installed the protocol 2 wrapper. Restart the OctoPrint service to refresh group membership."
echo "Flash matching firmware separately after review. No firmware was uploaded by this installer."
