# rfid-copier

A small serial-driven tool for reading and copying **MIFARE Classic 1K** RFID
cards using an **Arduino Nano** and an **RC522** module. You drive it over the
USB serial console with simple one-word commands.

Built with [PlatformIO](https://platformio.org/) and the
[`miguelbalboa/MFRC522`](https://github.com/miguelbalboa/rfid) library.

## Hardware

- Arduino Nano (ATmega328P)
- RC522 RFID reader/writer module (13.56 MHz)
- MIFARE Classic 1K cards/tags

### Wiring (RC522 → Nano)

| RC522 pin | Nano pin |
|-----------|----------|
| SDA / SS  | D10      |
| SCK       | D13      |
| MOSI      | D11      |
| MISO      | D12      |
| RST       | D9       |
| 3.3V      | **3.3V** (not 5V!) |
| GND       | GND      |

> ⚠️ The RC522 is a **3.3V** device. Power it from the Nano's 3.3V pin. The
> SPI data pins tolerate the 5V logic fine in practice.

## Build & flash

```sh
pio run                 # compile
pio run -t upload       # flash the Nano
pio device monitor -b 115200   # open the serial console
```

## Usage

Open the serial monitor at **115200 baud** and type commands (one per line):

| Command    | What it does |
|------------|--------------|
| `help`     | List commands. |
| `info`     | Show the UID, SAK and type of the presented card. |
| `read`     | Read a card into the in-RAM buffer and print a hex dump. |
| `dump`     | Print the current buffer (`T` marks sector-trailer blocks). |
| `write`    | Write the buffer's **data blocks** onto a target card, with read-back verify. |
| `writeuid` | Attempt to write the UID onto the target card — **magic cards only** (see below). |
| `clear`    | Discard the buffer. |

Typical flow: put the source card on the reader and run `read`, then swap in a
blank target card and run `write`.

## What "copy" means here (important)

A MIFARE Classic card has two kinds of content:

- **Data blocks** — the 16 sectors of user data. These can be read and
  rewritten freely as long as the card uses the default key
  (`FF FF FF FF FF FF`), which blank/cheap cards almost always do. `read` +
  `write` handle these.
- **The UID** — the factory serial number in block 0. On a **genuine** card
  this is **read-only** and cannot be changed. A true 1:1 clone (same UID)
  therefore requires a special **"magic" card** (Gen1a/Gen2) whose UID is
  writable. The `writeuid` command attempts this: it succeeds on a magic card
  and fails harmlessly on a normal one.

`write` deliberately **skips block 0 and the sector trailers** (which hold keys
and access bits) to avoid bricking a card.

## Notes & scope

This is a personal learning/hobby project for working with **your own** cards —
understanding how MIFARE Classic storage is laid out and how the RC522 reads and
writes it. Sectors protected with non-default keys are reported as auth failures
and left untouched.

## License

MIT — see [LICENSE](LICENSE).
