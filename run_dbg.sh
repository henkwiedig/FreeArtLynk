#!/bin/sh

telnetd -p 1337 -l /bin/sh
/etc/usb_gadget_configfs.sh rndis+uart 0 dwc2_9311 0x1d6b 0x0101
ar_logcat -f /tmp/usrlog/tx.log 122880 10 &
/usrdata/arlink_fpv -i 192.168.3.101 -p 5600 > /tmp/run_dbg.log 2>&1 &
echo "arlink_fpv pid=$!" >> /tmp/run_dbg.log
ota_upgrade &
