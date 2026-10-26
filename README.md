# VRC WinForce

## Weiterentwicklung / Continued development

Die PC-Funktionen werden in [VRC HydroLink](https://github.com/Hydroman-dot/VRC-HydroLink) als integriertes Modul weiterentwickelt. Dieses Einzelprojekt bleibt als historischer Stand erhalten und ist zur Archivierung vorgesehen. Bestehende Downloads und Anleitungen beziehen sich auf diesen alten Stand. Offene Unterschiede und Hardware-Abnahmen: [HydroLink-Funktionsvergleich](https://github.com/Hydroman-dot/VRC-HydroLink/blob/main/docs/FEATURE-PARITY.md).

PC development continues in VRC HydroLink. This standalone project is retained as a historical reference and is scheduled for archival. Existing downloads and instructions describe the legacy version; full feature parity and hardware validation are not claimed.

VRC WinForce is a modular VR wind controller for Windows and ESP32. It turns live VR and fitness signals into real airflow using up to three independently wired 4-pin PWM PC fans.

**Current PC version:** 1.8.2  
**Current ESP32 firmware:** 1.3.3

## What it does

```text
VRChat / VRTI / FitOSC / Heart Rate / PhysBone
                    |
                    v
             VRC WinForce PC
                    |
                 UDP/Wi-Fi
                    |
                    v
                ESP32
              /    |    \
           Fan 1 Fan 2 Fan 3
```

The Windows application reads supported inputs, calculates a wind level, applies source priority/calibration/fade behaviour, and sends commands to the ESP32. The ESP32 generates 25 kHz PWM signals for three independent fan channels.

The three channels are already electrically independent. The current PC app normally sends the same level to all three fans; directional wind is planned for a later version.

## Supported inputs

- Manual control
- VRChat OSC movement
- VRTI treadmill speed
- VRTI AutoWalk
- FitOSC treadmill/walking data
- VRChat PhysBone wind sensor input (`WinForceSensor_Angle`)
- Heart rate via VRCOSC Bluetooth Heartrate WebSocket
- Pulsoid heart-rate input
- Configurable source priority
- Per-source calibration
- Per-source fade-out curves
- Global fan pause/resume

## PC application

PC v1.8.2 adds the VRC WinForce application icon to the window/taskbar and desktop shortcut. The global **Pause fans** button from v1.8.1 remains available. While paused, fan output is forced to 0% while VRTI, OSC, FitOSC, PhysBone and heart-rate inputs continue running. Resuming starts again from 0% with the normal rise smoothing.

## ESP32 firmware

Firmware 1.3.3 includes:

- Wi-Fi setup
- UDP control
- discovery/status telemetry
- web diagnostics
- direct PWM tests
- three independent PWM outputs
- three display-only tachometer inputs
- failsafe to 0%
- Arduino OTA
- browser-based firmware upload

### Pin assignment

| Function | GPIO |
|---|---:|
| Fan 1 PWM | 4 |
| Fan 2 PWM | 16 |
| Fan 3 PWM | 17 |
| Fan 1 TACH | 25 |
| Fan 2 TACH | 26 |
| Fan 3 TACH | 27 |

PWM frequency: **25 kHz**.

Tachometer values are **telemetry only** and never influence fan control. Each tach input should have its own **10 kΩ pull-up to 3.3 V**.

## UDP protocol

Default ports:

| Purpose | Port |
|---|---:|
| Commands | UDP 4210 |
| Discovery | UDP 4211 |

Supported commands:

```text
FAN <percent> <sequence>
FANS <fan1%> <fan2%> <fan3%> <sequence>
FAN1 <percent> <sequence>
FAN2 <percent> <sequence>
FAN3 <percent> <sequence>
```

## Windows installation / Historischer PC-Stand

Für neue Installationen wird [VRC HydroLink](https://github.com/Hydroman-dot/VRC-HydroLink) verwendet.

In diesem Repository ist der alte PC-Code 1.8.2 als komprimierte Bootstrap-Dateien unter `bootstrap/appgz.00` bis `appgz.04` abgelegt. Der früher genannte Ordner `pc/` und sein Installer sind im aktuellen Repository nicht vorhanden. Die frühere Installationsanweisung ist daher nicht direkt ausführbar.

Der Workflow [Bootstrap VRC WinForce PC files](.github/workflows/bootstrap.yml) beschreibt die Wiederherstellung des historischen Codes und die Erzeugung des Installers. Er verändert das Repository und ist kein fertiger Download. Die Bootstrap-Dateien bleiben zur Nachvollziehbarkeit erhalten.

For new installations, use HydroLink. The legacy PC source is retained as compressed bootstrap payloads; the previously documented installer is not present in this checkout.

## ESP32

Target hardware:

- classic ESP32 / WROOM-32 / DevKitC
- Arduino ESP32 Core 3.x
- 4 MB flash

Recommended settings:

```text
Board: ESP32 Dev Module
CPU Frequency: 240 MHz
Flash Frequency: 80 MHz
Flash Mode: QIO
Flash Size: 4 MB
```

Use an OTA-capable partition layout.

For browser OTA, upload the normal compiled sketch file:

```text
VRC_WinForce_ESP32.ino.bin
```

Do not use `bootloader.bin`, `partitions.bin`, or `merged.bin` for normal browser OTA.

## Hardware

The fan motor current comes directly from the 12 V supply. Each fan PWM control line uses its own transistor/open-collector stage. See [docs/hardware-wiring.md](docs/hardware-wiring.md).

## Safety

If valid PC commands stop for several seconds, the ESP32 forces fan output to 0%.

Firmware updates also force PWM to 0%.

## Historical roadmap (continued in HydroLink)

- Directional wind using the three independent fan channels
- Weitere Entwicklung des Unity-/Modular-Avatar-Sensors (historisches Paket bereits unter [unity/](unity/) vorhanden)
- PC-driven ESP32 firmware updater
- Additional game integrations

## License

MIT. See [LICENSE](LICENSE).

## KI-Hinweis / AI disclosure

Dieses Projekt wird mit Unterstützung künstlicher Intelligenz entwickelt und dokumentiert. Der Hinweis beschreibt die Entwicklung und bedeutet nicht, dass das Programm selbst KI-Funktionen verwendet.

This project is developed and documented with assistance from artificial intelligence. This disclosure describes the development process; it does not imply that the software itself uses AI features.
