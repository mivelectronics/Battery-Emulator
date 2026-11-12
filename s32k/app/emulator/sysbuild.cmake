# SPDX-License-Identifier: GPL-3.0-or-later
# Sysbuild builds (tools/build_ota.sh): the board id in the application image (TLV 0xA0) is the name of
# the board it is built for, which be_ota.c compares with CONFIG_BOARD: an update for another board is
# refused. Set here last, over the conf files.
set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS "--custom-tlv 0xA0 ${BOARD}")
