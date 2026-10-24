#!/bin/zsh
# iCloud (Optimize Storage) evicted repo files to "dataless" when the disk filled up; reads from sandboxed shells then hang.
# Run from the user's Terminal: reads every dataless file so macOS downloads it again, then moves the repo out of the
# iCloud-managed Documents folder (leaving a symlink at the old path) so it cannot be evicted again.
set -u
SRC=/Users/neto/Documents/Napple/Napple
DST=/Users/neto/dev/Napple
cd "$SRC" || exit 1
for pass in 1 2 3; do
  n=$(find . -path ./target -prune -o -path ./work -prune -o -type f -flags +dataless -print | wc -l | tr -d ' ')
  echo "pass $pass: $n dataless files"
  [ "$n" = 0 ] && break
  find . -path ./target -prune -o -path ./work -prune -o -type f -flags +dataless -print0 | xargs -0 -n 10 cat > /dev/null
done
n=$(find . -path ./target -prune -o -path ./work -prune -o -type f -flags +dataless -print | wc -l | tr -d ' ')
echo "remaining dataless: $n"
[ "$n" = 0 ] || { echo "not moving: some files still dataless"; exit 2; }
echo "materialized OK"
