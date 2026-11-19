#!/bin/sh
# green-capture.sh [label]: snapshot the display pipeline (run as root, screen ON)
grep -q "^enabled" /sys/class/drm/card0-DSI-1/enabled || { echo "turn the screen on first"; exit 1; }
T=$(date +%m%d-%H%M%S)-${1:-capture}
D=/home/alarm/green/$T; mkdir -p $D
cat /sys/class/drm/card0-DSI-1/enabled > $D/screen 2>&1
cat /sys/kernel/debug/dri/0/state > $D/dri-state.txt 2>&1
cat /sys/kernel/debug/dri/0/framebuffer > $D/dri-fb.txt 2>&1
sh /home/alarm/dqedump.sh $D/dqe.txt
for b in 19470000 19480000 19490000 194a0000; do /home/alarm/mmrd 0x$b 0x1000; done > $D/decon.txt
dmesg | tail -300 > $D/dmesg.txt
tail -200 /home/alarm/.local/state/ztmetro.log > $D/ztmetro.txt 2>/dev/null
chown -R alarm:alarm /home/alarm/green
echo "saved $D"
