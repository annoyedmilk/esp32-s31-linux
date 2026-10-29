# SPDX-License-Identifier: MIT
# The desktop on the panel: the login on tty1 starts tmux, and its first
# window shows the motd with the tools.  The next login attaches to the same
# session, so its windows stay.  Without tmux, or when it fails, the login
# stays a usual shell.  Kernel messages then go to tty2 (Alt-F2), not over
# the desktop; the open allocates tty2, else the kernel ignores the redirect.
if [ "$(tty)" = /dev/tty1 ] && [ -z "$TMUX" ] && command -v tmux >/dev/null; then
	: </dev/tty2 && setlogcons 2
	tmux new-session -A -s desk "cat /etc/motd; exec $SHELL -l" && exit
fi
