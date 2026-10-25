#!/bin/bash
# tools/long_monitor.sh <pid> <logfile> [interval-s]
# Samples a running napple-runtime every N seconds (default 5): elapsed, RSS/VSZ (KB), threads, %CPU, plus the window title (fps) when a window exists.
# Type "mark" lines: `echo slow >> <logfile>.mark` adds a MARK row at the next sample. Stops when the pid exits.
PID="$1"; LOG="$2"; IV="${3:-5}"; T0=$(date +%s)
echo "# t_s rss_kb vsz_kb threads cpu% title" > "$LOG"
while kill -0 "$PID" 2>/dev/null; do
  T=$(( $(date +%s) - T0 ))
  R=$(ps -o rss=,vsz=,%cpu= -p "$PID" | awk '{print $1, $2, $3}')
  TH=$(ps -M -p "$PID" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
  TI=$(osascript -e 'tell application "System Events" to get name of every window of (first process whose unix id is '"$PID"')' 2>/dev/null)
  echo "$T $R $TH ${TI:--}" >> "$LOG"
  if [ -s "$LOG.mark" ]; then echo "MARK t=$T $(cat "$LOG.mark" | tr '\n' ' ')" >> "$LOG"; : > "$LOG.mark"; fi
  sleep "$IV"
done
