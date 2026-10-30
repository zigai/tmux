#!/bin/sh

# OSC 22 keeps a pointer shape stack for each of the main and alternate
# screens and answers queries. Replies are echoed into the pane by cat -v.

. ./input-common.inc

query()
{
	name=$1
	seq=$2

	start_cmd "$name" 80 3 \
	    "stty raw -echo; printf '$seq'; printf X; exec cat -v"
}

Q='\033]22;?__current__\033\\'

query osc-22-empty "$Q"
check_capture osc-22-empty 'X^[]22;0^[\'

query osc-22-set "\\033]22;pointer\\033\\\\$Q"
check_capture osc-22-set 'X^[]22;pointer^[\'

query osc-22-set-bel '\033]22;=wait\007\033]22;?__current__\007'
check_capture osc-22-set-bel 'X^[]22;wait^G'

query osc-22-alias "\\033]22;hand2\\033\\\\$Q"
check_capture osc-22-alias 'X^[]22;pointer^[\'

query osc-22-unknown "\\033]22;text\\033\\\\\\033]22;no-such-shape\\033\\\\$Q"
check_capture osc-22-unknown 'X^[]22;text^[\'

query osc-22-reset "\\033]22;text\\033\\\\\\033]22;\\033\\\\$Q"
check_capture osc-22-reset 'X^[]22;0^[\'

query osc-22-push "\\033]22;>wait,grab\\033\\\\$Q\\033]22;<\\033\\\\$Q\\033]22;<\\033\\\\$Q\\033]22;<\\033\\\\$Q"
check_capture osc-22-push 'X^[]22;grab^[\^[]22;wait^[\^[]22;0^[\^[]22;0^[\'

query osc-22-replace "\\033]22;>wait,grab\\033\\\\\\033]22;help\\033\\\\\\033]22;<\\033\\\\$Q"
check_capture osc-22-replace 'X^[]22;wait^[\'

# Pushing more than the stack holds drops the oldest shape.
seq='\033]22;>help,wait'
i=0
while [ "$i" -lt 15 ]; do
	seq="$seq,text"
	i=$((i + 1))
done
seq="$seq\\033\\\\"
i=0
while [ "$i" -lt 15 ]; do
	seq="$seq\\033]22;<\\033\\\\"
	i=$((i + 1))
done
query osc-22-overflow "$seq$Q\\033]22;<\\033\\\\$Q"
check_capture osc-22-overflow 'X^[]22;wait^[\^[]22;0^[\'

query osc-22-alternate "\\033]22;pointer\\033\\\\\\033[?1049h$Q\\033]22;move\\033\\\\$Q\\033[?1049l$Q\\033[?1049h$Q\\033[?1049l"
check_capture osc-22-alternate 'X^[]22;0^[\^[]22;move^[\^[]22;pointer^[\^[]22;move^[\'

query osc-22-ris "\\033]22;pointer\\033\\\\\\033c$Q"
check_capture osc-22-ris 'X^[]22;0^[\'

query osc-22-respawn "\\033]22;pointer\\033\\\\$Q"
check_capture osc-22-respawn 'X^[]22;pointer^[\'
$TMUX respawn-pane -k -t osc-22-respawn: \
    "stty raw -echo; printf '$Q'; printf X; exec cat -v" || exit 1
check_capture osc-22-respawn 'X^[]22;0^[\'

query osc-22-support '\033]22;?pointer,zoom-out,hand2,no-such-shape,__default__,__grabbed__\033\\'
check_capture osc-22-support 'X^[]22;1,1,0,0,default,default^[\'

exit $exit_status
