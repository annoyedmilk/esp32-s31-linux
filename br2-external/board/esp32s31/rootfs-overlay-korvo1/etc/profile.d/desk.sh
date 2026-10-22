# SPDX-License-Identifier: MIT
# The desktop on the panel: the login on tty1 starts tmux.  The next login
# attaches to the same session, so its windows stay.  Without tmux, or when
# it fails, the login stays a usual shell.
if [ "$(tty)" = /dev/tty1 ] && [ -z "$TMUX" ] && command -v tmux >/dev/null; then
	tmux new-session -A -s desk && exit
fi
