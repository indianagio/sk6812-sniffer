# SK6812 Sniffer — ESP32-H2-DevKitM-1

Intercetta il segnale dati di una strip SK6812/WS2812 tramite ESP32-H2 (RMT + DMA)
e salva i frame RGBW sul PC via USB seriale.

> **Framework**: ESP-IDF | **Tool**: PlatformIO via [pioarduino](https://github.com/pioarduino/platform-espressif32)

## Hardware

```
Controller
    ├── GND ─────────────────────────────── GND  (ESP32-H2)
    └── DATA ── 10kΩ ──┬── GPIO5 (RMT RX)
                        │
                       20kΩ
                        │
                       GND
```

Partitore di tensione 10kΩ/20kΩ: abbassa il livello logico da 5V a ~3.3V.

### Quale USB-C usare

| Porta sulla board | Chip        | Uso                                  |
|-------------------|-------------|--------------------------------------|
| **UART**          | CP2102N     | ✅ Flash + monitor seriale (questa!) |
| **USB**           | USB CDC H2  | ⛔ Non usare per questo progetto     |

Collega sempre il PC alla porta **UART**.

## Setup PlatformIO (pioarduino)

Il supporto ESP32-H2 con IDF5 richiede **pioarduino** al posto del plugin PlatformIO standard.

### Opzione A — Estensione pioarduino in VSCode
1. Disinstalla l'estensione **PlatformIO IDE** (se presente)
2. Installa l'estensione **pioarduino** dal marketplace VSCode
3. Apri il progetto — pioarduino rileva `platformio.ini` automaticamente

### Opzione B — PlatformIO con platform custom
Se preferisci tenere PlatformIO IDE, la `platform` nel `platformio.ini` già punta
al pacchetto pioarduino via URL:
```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```
Funziona anche con PlatformIO standard senza sostituire l'estensione.

## Configurazione porta

Modifica `upload_port` e `monitor_port` in `platformio.ini`:

| OS      | Porta tipica                  |
|---------|-------------------------------|
| Linux   | `/dev/ttyUSB0`                |
| macOS   | `/dev/cu.usbserial-XXXX`      |
| Windows | `COM3` (vedi Device Manager)  |

## Build & Flash

```bash
# Da VSCode: click Build (✓) poi Upload (→)
# Da terminale pioarduino/pio:
pio run -t upload
pio device monitor
```

Cambia `NUM_LEDS` in `src/main.c` (default: 60).

## Cattura frame sul PC

```bash
pip install pyserial

# Cattura 200 frame
python tools/capture.py --port /dev/ttyUSB0 --output frames.bin --count 200

# Cattura illimitata (Ctrl+C per fermare)
python tools/capture.py --port /dev/ttyUSB0 --output frames.bin
```

## Decodifica

```bash
# Riepilogo statistiche
python tools/decode.py --input frames.bin --summary

# Esporta JSON
python tools/decode.py --input frames.bin --format json --output frames.json

# Esporta CSV
python tools/decode.py --input frames.bin --format csv --output frames.csv
```

## Struttura repo

```
sk6812-sniffer/
├── src/
│   └── main.c              # Firmware ESP-IDF (RMT+DMA + UART output)
├── tools/
│   ├── capture.py          # Cattura frame da seriale → .bin
│   └── decode.py           # Decodifica .bin → JSON / CSV
├── platformio.ini          # Config PlatformIO (pioarduino)
├── CMakeLists.txt          # Richiesto da ESP-IDF
├── sdkconfig.defaults      # Preset ESP32-H2: UART console, RMT IRAM
└── README.md
```

## Formato file `frames.bin`

| Campo      | Tipo      | Note                          |
|------------|-----------|-------------------------------|
| `SK6812CAP`| 9 byte    | Magic                         |
| version    | uint16 LE | `1`                           |
| timestamp  | float64   | Secondi dall'inizio cattura   |
| num_leds   | uint16 LE | Numero LED nel frame          |
| payload    | byte[]    | `N × 4` byte in ordine R G B W|
| *(ripeti)* | ...       | Frame successivi              |
