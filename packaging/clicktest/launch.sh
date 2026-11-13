#!/bin/sh
mydir=$(cd "$(dirname "$0")" && pwd)
cd "$mydir" || exit 1
exec > "$mydir/launch.log" 2>&1

echo "== Before audioserver stop"
ps | grep -E 'audioserver|keymon' | grep -v grep
grep LineOut /proc/mi_modules/mi_ao/mi_ao0

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

# Gives this app exclusive access to the vendor audio device. Onion restarts
# the audio server itself the next time it launches something.
sh /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh
sleep 1

echo "== After audioserver stop"
ps | grep audioserver | grep -v grep
grep LineOut /proc/mi_modules/mi_ao/mi_ao0

touch /tmp/stay_awake

unset LD_PRELOAD
export LD_LIBRARY_PATH="$mydir/lib:/customer/lib:/config/lib:$LD_LIBRARY_PATH"
export SDL_AUDIODRIVER=miao

chmod +x ./clicktest
./clicktest
echo "== clicktest exited with status $?"

rm -f /tmp/stay_awake
