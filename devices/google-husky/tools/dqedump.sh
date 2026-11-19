#!/bin/sh
# dqedump.sh OUT: DQE 0x194D0000..+0xF000 page by page
: > $1
for p in 0 1 2 3 4 5 6 7 8 9 a b c d e; do
  echo "page $p" > /tmp/dqe-progress
  /home/alarm/mmrd 0x194D${p}000 0x1000 >> $1 || exit 1
  sync
done
echo done > /tmp/dqe-progress
