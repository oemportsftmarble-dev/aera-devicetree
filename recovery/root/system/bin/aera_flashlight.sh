#!/system/bin/sh

# TODO: Add torch_3 node to actually show max_brightness as torch_0 alone is not enough

DIR="/tmp/flashlight"
STATE_FILE="$DIR/brightness"
MAX_FILE="$DIR/max_brightness"

TORCH_MA=40

set_torch() {
    for node in /sys/class/leds/led:torch_0/brightness; do
        [ -e "$node" ] && echo "$1" > "$node" 2>/dev/null
    done
}

set_switch() {
    for node in /sys/class/leds/led:switch_0/brightness; do
        [ -e "$node" ] && echo "$1" > "$node" 2>/dev/null
    done
}

rm -rf "$DIR"
mkdir -p "$DIR"

echo 1 > "$MAX_FILE"
echo 0 > "$STATE_FILE"
chmod 666 "$MAX_FILE" "$STATE_FILE"

set_switch 0
set_torch 0

LAST=0

while true; do
    STATE=""
    IFS= read -r STATE < "$STATE_FILE"

    case "$STATE" in
        1|[1-9]*)
            if [ "$LAST" != "1" ]; then
                set_torch "$TORCH_MA"
                set_switch 1
                LAST=1
            fi
            ;;
        *)
            if [ "$LAST" != "0" ]; then
                set_switch 0
                set_torch 0
                LAST=0
            fi
            ;;
    esac
    sleep 0.1
done
