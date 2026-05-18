# SK6812 Sniffer — ESP32-H2

Intercetta il segnale dati di una strip SK6812/WS2812 tramite ESP32-H2 (RMT + DMA)
e salva i frame RGBW sul PC via USB seriale.

## Hardware

```
Controller
    ├── GND ──────────────────────────────── GND ESP32-H2
    └── DATA ── 10kΩ ──┬── GPIO5 ESP32-H2
                        │
                       20kΩ
                        │
                       GND
```

> Il partitore 10kΩ / 20kΩ converte il livello logico da 5V a ~3.3V.

## Requisiti

- **ESP-IDF v5.2+** — [Guida installazione](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/)
- **Python 3.8+** con `pyserial`: `pip install pyserial`

## Build & Flash

```bash
cd sk6812-sniffer
idf.py set-target esp32h2
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Cambia `NUM_LEDS` in `main/main.c` per adattarlo alla tua strip (default: 60).

## Cattura

```bash
# Cattura 200 frame e salvali su frames.bin
python tools/capture.py --port /dev/ttyACM0 --output frames.bin --count 200

# Cattura illimitata (Ctrl+C per fermare)
python tools/capture.py --port /dev/ttyACM0 --output frames.bin
```

## Decodifica

```bash
# Riepilogo
python tools/decode.py --input frames.bin --summary

# Esporta in JSON
python tools/decode.py --input frames.bin --format json --output frames.json

# Esporta in CSV
python tools/decode.py --input frames.bin --format csv --output frames.csv
```

## Struttura repo

```
sk6812-sniffer/
├── main/
│   ├── main.c          # Firmware ESP-IDF (RMT+DMA receiver + UART output)
│   └── CMakeLists.txt
├── tools/
│   ├── capture.py      # Cattura frame da seriale → .bin
│   └── decode.py       # Decodifica .bin → JSON / CSV
├── CMakeLists.txt
├── sdkconfig.defaults  # Preset per ESP32-H2 + USB CDC
└── README.md
```

## Formato file binario (`frames.bin`)

| Campo      | Tipo      | Descrizione                   |
|------------|-----------|-------------------------------|
| magic      | 9 byte    | `SK6812CAP`                   |
| version    | uint16 LE | `1`                           |
| timestamp  | float64   | Secondi dall'avvio cattura    |
| num_leds   | uint16 LE | Numero LED nel frame          |
| payload    | byte[]    | `num_leds × 4` byte (R G B W) |
| *(ripeti)* | ...       | Frame successivi              |
