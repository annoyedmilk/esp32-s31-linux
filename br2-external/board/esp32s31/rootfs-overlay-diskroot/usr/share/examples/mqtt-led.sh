#!/bin/sh
# SPDX-License-Identifier: MIT
# Set the RGB LED from MQTT: each message "R G B" (0 to 255) on the topic
# esp32s31/MQTT_ID/led sets the color; "0 0 0" turns it off.  Other
# messages are refused.  The broker is in /etc/default/mqtt.  On the
# computer:
#
#   mosquitto_pub -h BROKER -t esp32s31/esp32s31/led -m "0 32 0"
#
#   mqtt-led.sh

. /etc/default/mqtt
set -- -h "$MQTT_HOST" -p "$MQTT_PORT" -i "$MQTT_ID-led"
[ -n "$MQTT_USER" ] && set -- "$@" -u "$MQTT_USER" -P "$MQTT_PASS"
[ -n "$MQTT_CAFILE" ] && set -- "$@" --cafile "$MQTT_CAFILE"

# 0 to 255, decimal digits only
is_byte()
{
	case $1 in
	'' | *[!0-9]* | ????*) return 1 ;;
	esac
	[ "$1" -le 255 ]
}

mosquitto_sub "$@" -t "esp32s31/$MQTT_ID/led" | while read -r r g b extra; do
	if [ -n "$extra" ] || ! is_byte "$r" || ! is_byte "$g" || ! is_byte "$b"; then
		echo "mqtt-led: refused: $r $g $b $extra" >&2
		continue
	fi
	/usr/share/examples/led.sh "$r" "$g" "$b"
done
