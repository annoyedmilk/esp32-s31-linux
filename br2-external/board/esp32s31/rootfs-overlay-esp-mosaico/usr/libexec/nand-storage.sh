# SPDX-License-Identifier: MIT
# Shared by early init, setup and swap. MTD and UBI numbers are not stable.

nand_mtd()
{
	for path in /sys/class/mtd/mtd[0-9]*; do
		[ -f "$path/name" ] || continue
		[ "$(cat "$path/name")" = "$1" ] || continue
		[ "$(cat "$path/type")" = nand ] || return 1
		[ "$(cat "$path/size")" = "$2" ] || return 1
		printf '%s\n' "${path##*/}"
		return 0
	done
	return 1
}

nand_ubi()
{
	for path in /sys/class/ubi/ubi[0-9]*; do
		[ -f "$path/mtd_num" ] || continue
		[ "$(cat "$path/mtd_num")" = "${1#mtd}" ] || continue
		printf '%s\n' "${path##*/}"
		return 0
	done
	return 1
}
