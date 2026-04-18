#!/bin/sh
# Kill everything, feed watchdog, deploy new binary, start it
killall -9 arlink_daemon arlink_fpv 2>/dev/null

# Feed watchdog to prevent reboot
(while true; do printf '\0' > /dev/watchdog0; sleep 2; done) &

sleep 1

# Download new binary
# start python -m http.server 8080 locally
wget -q http://192.168.3.101:8080/arlink_stream -O /usrdata/arlink_fpv
chmod +x /usrdata/arlink_fpv

# Verify
SIZE=$(wc -c < /usrdata/arlink_fpv)
echo "DEPLOY: binary size=$SIZE" > /tmp/deploy.log

# Start with correct args
/usrdata/arlink_fpv -i 192.168.3.101 -p 5600 >> /tmp/deploy.log 2>&1 &
echo "DEPLOY: started pid=$!" >> /tmp/deploy.log

sleep 5
echo "DEPLOY: frames=$(grep -c 'frames' /tmp/arlink.log 2>/dev/null)" >> /tmp/deploy.log
cat /tmp/deploy.log
