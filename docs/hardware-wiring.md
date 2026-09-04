# Hardware wiring

PWM:
- Fan 1: GPIO4 -> 1kΩ -> transistor base
- Fan 2: GPIO16 -> 1kΩ -> transistor base
- Fan 3: GPIO17 -> 1kΩ -> transistor base

Each fan PWM channel uses its own NPN/open-collector stage. Fan motor current comes directly from the 12 V power supply.

TACH:
- Fan 1 -> GPIO25
- Fan 2 -> GPIO26
- Fan 3 -> GPIO27

Each tach input gets its own 10 kΩ pull-up to 3.3 V. Do not pull tach lines to 5 V or 12 V.

RPM is telemetry only and does not control PWM.
