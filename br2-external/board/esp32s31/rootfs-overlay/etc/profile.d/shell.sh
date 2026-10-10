# SPDX-License-Identifier: MIT
# For interactive shells: a prompt with the directory and short names for ls.

case "$-" in
*i*) ;;
*) return ;;
esac

PS1='\u@\h:\w\$ '
alias ll='ls -l'
alias la='ls -la'

# bash writes its history only at exit, and the board is often reset.
[ -n "$BASH_VERSION" ] && PROMPT_COMMAND='history -a'
