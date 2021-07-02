#!/bin/sh
appstream-util validate-relax firmware.metainfo.xml
echo -n "hello world" > firmware.bin
gcab --create --nopath firmware.cab firmware.bin firmware.metainfo.xml
