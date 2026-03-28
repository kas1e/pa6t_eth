#ifndef VERSION_H
#define VERSION_H

#define DEVICE_VERSION   1
#define DEVICE_REVISION  0
#define DEVICE_DATE      "08.03.2026"
#define DEVNAME          "pa6t_eth.device"

#define STR(x)  #x
#define XSTR(x) STR(x)

#define DEVVER  DEVICE_VERSION
#define DEVREV  DEVICE_REVISION

#define DEVVERSIONSTRING \
    DEVNAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " (" DEVICE_DATE ")"

#define VERSION_LOG_STRING \
    DEVNAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " (" DEVICE_DATE ")"

#endif /* VERSION_H */
