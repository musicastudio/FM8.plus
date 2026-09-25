#!/bin/sh
# Run a command inside the logged-in GUI session and print its output: sh tools/mac/ingui.sh '<cmd>'
# AU hosts must, since the audio component registrar only lists third-party AUs to that session.
f=$(mktemp /tmp/ingui.XXXXXX) && mv "$f" "$f.command" && f="$f.command"
printf '#!/bin/sh\ncd "%s"\n( %s ) > "%s.out" 2>&1\necho $? > "%s.rc"\n' "$PWD" "$1" "$f" "$f" > "$f"
chmod +x "$f"
open -g "$f"
i=0; while [ ! -f "$f.rc" ] && [ $i -lt 600 ]; do sleep 1; i=$((i+1)); done
cat "$f.out"; rc=$(cat "$f.rc" 2>/dev/null || echo 124)
rm -f "$f" "$f.out" "$f.rc"
exit "$rc"
