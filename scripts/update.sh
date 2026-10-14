#!/bin/bash
# scripts/update.sh — Self-updater & upgrade utility for Crossfeed
#
# Usage:
#   scripts/update.sh             # Upgrade to latest version
#   scripts/update.sh --check     # Check if update is available without installing
#   scripts/update.sh --json      # Output check results in JSON format
#   scripts/update.sh --force     # Force reinstall/rebuild of latest version
set -euo pipefail

REPO="cecep-91/linux-crossfeed"
API_URL="https://api.github.com/repos/${REPO}/releases/latest"
DEFAULT_BIN="$HOME/.local/bin/crossfeed"

# Detect executable location or fallback
if command -v crossfeed >/dev/null 2>&1; then
    INSTALLED_BIN="$(command -v crossfeed)"
elif [ -x "$DEFAULT_BIN" ]; then
    INSTALLED_BIN="$DEFAULT_BIN"
else
    INSTALLED_BIN=""
fi

# Detect current version
CURRENT_VERSION="unknown"
if [ -n "$INSTALLED_BIN" ]; then
    # e.g. "pipewire-crossfeed 2.2.0 (standalone)" -> "2.2.0"
    CURRENT_VERSION=$("$INSTALLED_BIN" --version 2>/dev/null | awk '{print $2}' || echo "unknown")
fi
if [ "$CURRENT_VERSION" = "unknown" ] && [ -f "$(dirname "$0")/../src/main.cpp" ]; then
    CURRENT_VERSION=$(grep -m1 'pipewire-crossfeed' "$(dirname "$0")/../src/main.cpp" | sed -E 's/.*pipewire-crossfeed[[:space:]]+([0-9.]+).*/\1/' || echo "unknown")
fi

# Detect if running inside a Git repository
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." 2>/dev/null && pwd || echo "")"
IS_GIT_REPO=0
if [ -n "$ROOT_DIR" ] && [ -d "$ROOT_DIR/.git" ] && command -v git >/dev/null 2>&1; then
    if (cd "$ROOT_DIR" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
        IS_GIT_REPO=1
    fi
fi

# Fetch latest release info from GitHub API
fetch_latest_release() {
    local response=""
    if command -v curl >/dev/null 2>&1; then
        response=$(curl -fsSL -H "Accept: application/vnd.github.v3+json" "$API_URL" 2>/dev/null || true)
    elif command -v wget >/dev/null 2>&1; then
        response=$(wget -qO- --header="Accept: application/vnd.github.v3+json" "$API_URL" 2>/dev/null || true)
    fi

    if [ -z "$response" ]; then
        return 1
    fi

    LATEST_TAG=$(echo "$response" | grep -m1 '"tag_name":' | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/')
    LATEST_VERSION="${LATEST_TAG#v}"
    RELEASE_URL=$(echo "$response" | grep -m1 '"html_url":' | sed -E 's/.*"html_url":[[:space:]]*"([^"]+)".*/\1/')
}

# Version comparison: return 0 if $1 is strictly newer than $2
version_is_newer() {
    local ver1="$1"
    local ver2="$2"
    if [ "$ver1" = "$ver2" ]; then
        return 1
    fi
    [ "$(printf '%s\n' "$ver1" "$ver2" | sort -V | tail -n 1)" = "$ver1" ]
}

# Mode parsing
CHECK_ONLY=0
JSON_OUTPUT=0
FORCE_UPDATE=0

for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        --json) JSON_OUTPUT=1; CHECK_ONLY=1 ;;
        --force|-f) FORCE_UPDATE=1 ;;
        --help|-h)
            echo "Usage: $0 [--check] [--json] [--force]"
            echo "  --check   Check for updates without installing (exit 0=up-to-date, 10=update available)"
            echo "  --json    Output check results in JSON format"
            echo "  --force   Force reinstall or rebuild even if versions match"
            exit 0
            ;;
    esac
done

LATEST_TAG=""
LATEST_VERSION=""
RELEASE_URL=""
GIT_COMMITS_BEHIND=0

if ! fetch_latest_release; then
    if [ "$JSON_OUTPUT" = "1" ]; then
        echo "{\"error\":\"Failed to fetch latest release from GitHub API\",\"current\":\"$CURRENT_VERSION\"}"
    else
        echo "warning: Could not reach GitHub API to check for updates (offline or rate limited)." >&2
    fi
    exit 1
fi

HAS_UPDATE=0
if [ "$CURRENT_VERSION" != "unknown" ] && [ -n "$LATEST_VERSION" ]; then
    if version_is_newer "$LATEST_VERSION" "$CURRENT_VERSION"; then
        HAS_UPDATE=1
    fi
fi

# Check git commits behind if inside git repo
if [ "$IS_GIT_REPO" = "1" ]; then
    PREV_DIR="$PWD"
    cd "$ROOT_DIR"
    git fetch origin >/dev/null 2>&1 || true
    CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "development")
    REMOTE_REF="origin/$CURRENT_BRANCH"
    if git rev-parse --verify "$REMOTE_REF" >/dev/null 2>&1; then
        GIT_COMMITS_BEHIND=$(git rev-list --count "HEAD..$REMOTE_REF" 2>/dev/null || echo 0)
        if [ "$GIT_COMMITS_BEHIND" -gt 0 ]; then
            HAS_UPDATE=1
        fi
    fi
    cd "$PREV_DIR"
fi

# Handle --json output
if [ "$JSON_OUTPUT" = "1" ]; then
    echo "{"
    echo "  \"current\": \"$CURRENT_VERSION\","
    echo "  \"latest\": \"$LATEST_VERSION\","
    echo "  \"tag\": \"$LATEST_TAG\","
    echo "  \"has_update\": $([ "$HAS_UPDATE" = "1" ] && echo "true" || echo "false"),"
    echo "  \"is_git\": $([ "$IS_GIT_REPO" = "1" ] && echo "true" || echo "false"),"
    echo "  \"git_commits_behind\": $GIT_COMMITS_BEHIND,"
    echo "  \"release_url\": \"$RELEASE_URL\""
    echo "}"
    if [ "$HAS_UPDATE" = "1" ]; then
        exit 10
    else
        exit 0
    fi
fi

# Handle --check output
if [ "$CHECK_ONLY" = "1" ]; then
    echo "=================================================="
    echo "          Crossfeed Update Check                  "
    echo "=================================================="
    echo "Current installed version: $CURRENT_VERSION"
    echo "Latest release on GitHub:  $LATEST_VERSION ($LATEST_TAG)"
    if [ "$IS_GIT_REPO" = "1" ]; then
        echo "Git working tree:          Yes ($GIT_COMMITS_BEHIND commit(s) behind remote)"
    fi
    echo "--------------------------------------------------"
    if [ "$HAS_UPDATE" = "1" ]; then
        echo "Status: An update is available!"
        if [ "$IS_GIT_REPO" = "1" ] && [ "$GIT_COMMITS_BEHIND" -gt 0 ]; then
            echo "Action: Run 'crossfeed upgrade' or 'git pull && make && ./install.sh'"
        else
            echo "Action: Run 'crossfeed upgrade' to install v$LATEST_VERSION"
        fi
        echo "Release info: $RELEASE_URL"
        echo "=================================================="
        exit 10
    else
        echo "Status: Crossfeed is up to date!"
        echo "=================================================="
        exit 0
    fi
fi

# Perform Upgrade
echo "=================================================="
echo "          Crossfeed Upgrade Manager               "
echo "=================================================="
echo "Current version: $CURRENT_VERSION"
echo "Target version:  $LATEST_VERSION ($LATEST_TAG)"

if [ "$HAS_UPDATE" = "0" ] && [ "$FORCE_UPDATE" = "0" ]; then
    echo "Crossfeed is already up to date (version $CURRENT_VERSION)."
    echo "Use --force to reinstall or rebuild anyway."
    exit 0
fi

# Check if engine was running before upgrade so we can restart it seamlessly
WAS_RUNNING=0
if [ -n "$INSTALLED_BIN" ] && "$INSTALLED_BIN" status >/dev/null 2>&1; then
    WAS_RUNNING=1
    echo "==> Crossfeed engine is currently running (will restart after upgrade)"
fi

if [ "$IS_GIT_REPO" = "1" ]; then
    echo "==> Updating from local Git repository ($ROOT_DIR)..."
    cd "$ROOT_DIR"
    CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "development")
    echo "==> Pulling latest changes from branch '$CURRENT_BRANCH'..."
    git pull origin "$CURRENT_BRANCH"

    echo "==> Rebuilding crossfeed binary..."
    make clean
    make -j"$(nproc 2>/dev/null || echo 4)"

    echo "==> Installing updated binary and desktop files..."
    ./install.sh
else
    echo "==> Installing pre-built standalone release from GitHub..."
    ARCH="$(uname -m)"
    case "$ARCH" in
        x86_64) TAR_ARCH="x86_64" ;;
        aarch64|arm64) TAR_ARCH="aarch64" ;;
        *)
            echo "error: Unsupported CPU architecture '$ARCH' for prebuilt releases." >&2
            echo "Please clone the repository and run: make && ./install.sh" >&2
            exit 1
            ;;
    esac

    TARBALL_NAME="pipewire-crossfeed-${LATEST_VERSION}-linux-${TAR_ARCH}.tar.gz"
    DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${LATEST_TAG}/${TARBALL_NAME}"

    TMP_DIR=$(mktemp -d -t crossfeed-upgrade-XXXXXX)
    trap 'rm -rf "$TMP_DIR"' EXIT

    echo "==> Downloading $TARBALL_NAME..."
    if command -v curl >/dev/null 2>&1; then
        curl -fSL "$DOWNLOAD_URL" -o "$TMP_DIR/$TARBALL_NAME"
    else
        wget -q "$DOWNLOAD_URL" -O "$TMP_DIR/$TARBALL_NAME"
    fi

    echo "==> Extracting release archive..."
    tar -xzf "$TMP_DIR/$TARBALL_NAME" -C "$TMP_DIR"

    PKG_DIR=$(find "$TMP_DIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)
    if [ -z "$PKG_DIR" ] || [ ! -f "$PKG_DIR/install.sh" ]; then
        echo "error: Malformed release package." >&2
        exit 1
    fi

    echo "==> Running installer..."
    (cd "$PKG_DIR" && bash install.sh)
fi

# Restart engine if it was running
if [ "$WAS_RUNNING" = "1" ]; then
    echo "==> Restarting background Crossfeed engine..."
    if [ -x "$DEFAULT_BIN" ]; then
        "$DEFAULT_BIN" restart || true
    elif [ -n "$INSTALLED_BIN" ]; then
        "$INSTALLED_BIN" restart || true
    fi
fi

NEW_VERSION="unknown"
if [ -x "$DEFAULT_BIN" ]; then
    NEW_VERSION=$("$DEFAULT_BIN" --version 2>/dev/null | awk '{print $2}' || echo "unknown")
elif [ -n "$INSTALLED_BIN" ]; then
    NEW_VERSION=$("$INSTALLED_BIN" --version 2>/dev/null | awk '{print $2}' || echo "unknown")
fi

echo "=================================================="
echo "Upgrade complete! Crossfeed is now at version $NEW_VERSION."
echo "=================================================="
