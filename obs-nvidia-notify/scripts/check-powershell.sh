#!/usr/bin/env bash
# Catches the PowerShell parse errors that a Linux CI job would otherwise only discover on the
# Windows runner, minutes later and after everything else has already passed.
#
# It checks one thing, precisely, because that is the mistake that is easy to make and
# impossible to see: `"$name:"` inside a double-quoted string. PowerShell reads `$name:` as a
# scope or drive qualifier -- `$env:PATH` is the familiar case -- and if the colon is not
# followed by a valid variable-name character it is a hard parse error, not a warning.
#
# `$env:PATH` and `${name}:` are both fine and are not flagged.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

while IFS= read -r script; do
  printf '%-44s ' "${script#"$ROOT/"}"
  # A '$' + name + ':' where the next character cannot start a variable name.
  if matches="$(grep -nE '\$[A-Za-z_][A-Za-z0-9_]*:[^A-Za-z_{]' "$script")"; then
    echo "FAIL"
    echo "$matches" | while IFS= read -r line; do
      echo "    $line"
      echo "    ^ '\$name:' is a scope qualifier here. Write \${name} to delimit the name."
    done
    status=1
  else
    echo "OK"
  fi
done < <(find "$ROOT/scripts" -name '*.ps1' -print | sort)

exit $status
