
# Heltec ESP32 V3 (ESP32-S3 + SX1262) – GPS via LoRa P2P (Hardcoded)

Este pacote contém **dois projetos ESP-IDF** independentes:

- `tracker_tx/` – lê o **GPS NEO-6M (UART1 TX=GPIO43, RX=GPIO44)** e transmite **payload binário** via **LoRa SX1262** (Heltec WiFi LoRa 32 V3) a cada 5 s.
- `gateway_rx/` – recebe os pacotes via **LoRa P2P** e imprime no log.

## Pinos (Heltec WiFi LoRa 32 V3)
LoRa SX1262 (conforme exemplos/schematics da comunidade Heltec):
- **NSS/CS=GPIO8**, **SCK=GPIO9**, **MOSI=GPIO10**, **MISO=GPIO11**, **RST=GPIO12**, **BUSY=GPIO13**, **DIO1=GPIO14**.
- OLED integrado (não usado no código): **SDA=GPIO17**, **SCL=GPIO18**, **RST=GPIO21**.

GPS NEO-6M (UART1): **TX=GPIO43**, **RX=GPIO44** @ 9600 baud.

Caso seu board varie, ajuste os `#define` em `components/lora/lora.c` e em `components/gps/gps.c`.

## Parâmetros de rádio (P2P)
- **Frequência:** 915 MHz (BR/US)
- **BW:** 125 kHz, **SF:** 7, **CR:** 4/5, **TX:** 17 dBm, **Preamble:** 8
- Cabeçalho **explícito**, payload **variável**, **CRC ON**.

## Payload (12 bytes)
```
[ lat_i32 | lon_i32 | speed_i16 | flags_u8 | sats_u8 ]
```
- `lat_i32 = graus * 1e6`, `lon_i32 = graus * 1e6`, `speed_i16 = kmh * 10`

## Como compilar e gravar
1. Instale o **ESP-IDF v5+** e selecione o alvo: `idf.py set-target esp32s3`
2. Entre na pasta do projeto (`tracker_tx/` ou `gateway_rx/`)
3. Conecte a placa via USB-C (CP2102) e rode: `idf.py build flash monitor`

> Lembre-se de conectar **antena LoRa** antes de transmitir.

# V2 – Com acelerômetro BNO085 (gatilho de envio)

- `tracker_tx/`: usa **BNO085** (I2C em SDA=42, SCL=41) para detectar **movimento** (|a| com histerese) e, quando em movimento, lê o **GPS (UART1 TX=43 RX=44)** e envia via **LoRa (SX1262)**.
- `gateway_rx/`: recebe e imprime no terminal serial.

**LoRa pins (Heltec V3)**: NSS=8, SCK=9, MOSI=10, MISO=11, RST=12, BUSY=13, DIO1=14.**OLED (não usado)**: SDA=17, SCL=18, RST=21 (por isso o BNO vai em 42/41 para não conflitar com o OLED).Fontes de referência de pinagem Heltec V3 e periféricos: ver README do exemplo de comunidade e guia de pinos. 

Build: `idf.py set-target esp32s3` → `idf.py build flash monitor` em cada projeto.
