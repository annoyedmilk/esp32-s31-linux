#!/bin/sh
# SPDX-License-Identifier: MIT
# Send the uptime, the free memory and the Wi-Fi signal to the MQTT broker
# of /etc/default/mqtt, as report.sh does with HTTPS.  The retained
# discovery messages make them sensors in Home Assistant, without YAML.
# To send each 5 minutes:
#
#   echo '*/5 * * * * /usr/share/examples/mqtt-report.sh' | crontab -

. /etc/default/mqtt
set -- -h "$MQTT_HOST" -p "$MQTT_PORT" -i "$MQTT_ID"
[ -n "$MQTT_USER" ] && set -- "$@" -u "$MQTT_USER" -P "$MQTT_PASS"
[ -n "$MQTT_CAFILE" ] && set -- "$@" --cafile "$MQTT_CAFILE"

state=esp32s31/$MQTT_ID/state
up=$(cut -d' ' -f1 /proc/uptime)
mem=$(awk '$1 == "MemAvailable:" { print $2 }' /proc/meminfo)
rssi=$(iw dev wlan0 link 2>/dev/null | awk '$1 == "signal:" { print $2 }')

# Home Assistant: key, unit, device class, name.  read keeps "$@".
for s in "uptime s duration Uptime" "mem_kb kB data_size Memory available" \
	 "rssi dBm signal_strength Wi-Fi signal"; do
	read -r key unit class name <<EOF
$s
EOF
	mosquitto_pub "$@" -r -t "homeassistant/sensor/${MQTT_ID}_$key/config" -m \
"{\"name\":\"$name\",\"unique_id\":\"${MQTT_ID}_$key\",\"state_topic\":\"$state\",\
\"value_template\":\"{{ value_json.$key }}\",\"unit_of_measurement\":\"$unit\",\
\"device_class\":\"$class\",\"device\":{\"identifiers\":[\"$MQTT_ID\"],\
\"name\":\"$MQTT_ID\",\"model\":\"ESP32-S31\"}}" || exit 1
done
exec mosquitto_pub "$@" -t "$state" \
	-m "{\"uptime\":$up,\"mem_kb\":$mem,\"rssi\":${rssi:-null}}"
