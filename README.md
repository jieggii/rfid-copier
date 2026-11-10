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
| `writeuid` | Write the last-read card's UID onto the target card — **magic cards only** (see below). |
| `setuid <hex>` | Write a **custom UID** onto a magic card, e.g. `setuid 04 A1 B2 C3`. 4 or 7 bytes; separators optional. Reads the UID back to verify. |
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

## Writing a custom UID onto a magic card

Many simple access systems identify a card purely by its **UID** (serial
number), which isn't secret. To put a known UID onto a blank keyfob/sticker for
your own authorized use:

1. Read the source UID (e.g. with the NXP TagInfo phone app) — note whether it's
   **4 or 7 bytes**.
2. Get a **"magic" / UID-changeable** tag. Both flavors are supported — **Gen1a**
   ("backdoor") and **Gen2 / CUID** ("direct write"); the tool tries Gen1a first
   and falls back to Gen2. A 7-byte UID needs a **7-byte-capable** magic card —
   most cheap magic cards are 4-byte only and can't hold a 7-byte UID.
3. Present the magic card and run, e.g.: `setuid 04 A1 B2 C3 D4 E5 F6`
4. The command writes the UID and then re-reads the card to confirm what it now
   broadcasts (`MATCH` means success).

> ⚠️ **Encrypted cards can't be cloned.** Cards like **MIFARE DESFire** keep
> their data behind AES/3DES keys you don't hold, and authenticate with a live
> challenge-response — there is no static ciphertext to copy. Only the **UID**
> (not secret) can be duplicated, and only onto a magic card whose UID length
> matches. The RC522 may also fail to even detect a DESFire if its antenna is
> weak. Note too that the 7-byte UID path is best-effort: the library lays out
> block 0 in the 4-byte scheme, so it only yields a valid UID on a card that
> genuinely supports 7-byte writable UIDs — always check the read-back.

## Card memory & limitations

A MIFARE Classic 1K card is `16 sectors × 4 blocks × 16 bytes = 1024 bytes`.
Not all of it is free for your data:

- **Block 0** holds the UID / manufacturer data (read-only on genuine cards).
- The **last block of every sector** is a trailer (keys + access bits) — don't
  store data there.
- That leaves **~752 bytes** of usable read/write storage.

Rough endurance for genuine NXP silicon: **~10 year** data retention and
**~100k writes** per block (reads don't wear it). Kit/clone cards may not meet
these specs, and in practice a snapped antenna coil usually kills a card long
before the memory does.

> ⚠️ **Security note.** MIFARE Classic's CRYPTO1 cipher is broken — sector keys
> can be recovered in seconds with common tools. **Treat anything on one of
> these cards as publicly readable.** Don't rely on the card to protect secrets;
> if you must store sensitive data, encrypt it yourself first and store only the
> ciphertext.

## Notes & scope

This is a personal learning/hobby project for working with **your own** cards —
understanding how MIFARE Classic storage is laid out and how the RC522 reads and
writes it. Sectors protected with non-default keys are reported as auth failures
and left untouched.

## License

MIT — see [LICENSE](LICENSE).
