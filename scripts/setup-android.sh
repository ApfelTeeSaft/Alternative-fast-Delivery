#!/usr/bin/env bash
# Prepare the pinned Android SDK packages on the Linux CI runner.
set -euo pipefail

: "${ANDROID_HOME:?ANDROID_HOME must point to the Android SDK}"
: "${RUNNER_TEMP:?RUNNER_TEMP must point to the runner temporary directory}"
: "${GITHUB_ENV:?GITHUB_ENV must point to the workflow environment file}"

tool="$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager"
if [[ ! -x "$tool" ]]; then
    archive="$RUNNER_TEMP/android-tools.zip"
    curl --http1.1 --fail --location --show-error \
        --retry 3 --retry-all-errors --retry-delay 5 --retry-max-time 900 \
        --connect-timeout 20 --max-time 300 --speed-limit 1024 --speed-time 60 \
        https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip \
        --output "$archive"
    unzip -tq "$archive"
    unzip -qo "$archive" -d "$RUNNER_TEMP/afd-android-tools"
    tool="$RUNNER_TEMP/afd-android-tools/cmdline-tools/bin/sdkmanager"
fi

retry_sdkmanager() {
    local attempt status
    for attempt in 1 2 3; do
        if "$tool" --sdk_root="$ANDROID_HOME" "$@"; then
            return 0
        else
            status=$?
        fi
        if [[ "$attempt" == 3 ]]; then
            return "$status"
        fi
        echo "Android SDK setup failed; retrying (attempt $((attempt + 1))/3)." >&2
        sleep "$((attempt * 5))"
    done
}

(yes || true) | retry_sdkmanager --licenses > /dev/null
retry_sdkmanager 'platforms;android-36' 'build-tools;36.0.0' 'ndk;27.2.12479018'
printf 'ANDROID_NDK_HOME=%s\n' "$ANDROID_HOME/ndk/27.2.12479018" >> "$GITHUB_ENV"
