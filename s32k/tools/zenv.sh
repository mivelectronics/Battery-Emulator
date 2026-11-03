# source me: Zephyr env for this project (Git Bash)
# Workspace next to the repository clone (override with BE_WORKDIR)
export BE="${BE_WORKDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
export ZW="$BE/zw"
export ZEPHYR_BASE="$ZW/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$(cygpath -w "$BE/sdk/zephyr-sdk-1.0.1")"
export PATH="$ZW/.venv/Scripts:$PATH"
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.longpaths GIT_CONFIG_VALUE_0=true
