echo === TVBOX-HDMI-1 begin ===
echo before: outputmode=${outputmode} hdmimode=${hdmimode}
setenv outputmode 1080p60hz
setenv hdmimode 1080p60hz
echo after: outputmode=${outputmode} hdmimode=${hdmimode}
run storeboot
