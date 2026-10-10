#!/usr/bin/env bash
# Every app refuses an unknown option and a missing input directory with its usage and exit 2.
B=${1:-$(dirname "$0")/../build/native/apps}
fail=0
for app in book_replay checkpoint day_stats gen_fixture ingest itch_backtest itch_count itch_dump \
           pm_arb pm_exec pm_live pm_mm pm_record pm_stats store_cat transcode; do
  for args in "--bogus-flag" "/nonexistent/dir"; do
    case "$app $args" in
      "gen_fixture /nonexistent/dir"|"ingest /nonexistent/dir"|"itch_dump /nonexistent/dir"|"pm_exec /nonexistent/dir"|"pm_live /nonexistent/dir"|"pm_record /nonexistent/dir") continue;;  # no input directory first
    esac
    timeout 10 "$B/$app" $args </dev/null >/dev/null 2>&1
    rc=$?
    [ $rc -eq 2 ] || { echo "$app $args: exit $rc, want 2"; fail=1; }
  done
done
[ $fail -eq 0 ] && echo "cli: all apps refuse bad arguments"
exit $fail
