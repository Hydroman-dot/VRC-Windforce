# UDP protocol

Command port: `4210`  
Discovery port: `4211`

Commands:

```text
FAN <percent 0..100> <sequence>
FANS <fan1%> <fan2%> <fan3%> <sequence>
FAN1 <percent> <sequence>
FAN2 <percent> <sequence>
FAN3 <percent> <sequence>
STATUS
PING
```

Discovery/status packet types:
- `winforce_discovery`
- `winforce_status`

If valid commands stop for the configured timeout, the ESP32 forces fan output to 0%.
