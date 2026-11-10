/*
 * rfid-copier
 *
 * A small serial-driven tool for reading and copying MIFARE Classic 1K cards
 * with an Arduino Nano and an RC522 module.
 *
 * Wiring (RC522 -> Nano):
 *   SDA/SS -> D10   SCK -> D13   MOSI -> D11   MISO -> D12   RST -> D9
 *   3.3V   -> 3.3V (NOT 5V!)     GND -> GND
 *
 * Talk to it over the USB serial console at 115200 baud. Send one command per
 * line. Type `help` for the list.
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

// --- Pin configuration -----------------------------------------------------
constexpr uint8_t SS_PIN  = 10;
constexpr uint8_t RST_PIN = 9;

// --- Card geometry (MIFARE Classic 1K) -------------------------------------
constexpr uint8_t NUM_BLOCKS  = 64;  // 16 sectors * 4 blocks
constexpr uint8_t BLOCK_SIZE  = 16;  // bytes per block

MFRC522 mfrc522(SS_PIN, RST_PIN);

// Default factory key (blank cards almost always use this).
MFRC522::MIFARE_Key defaultKey = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// --- In-RAM copy of a card -------------------------------------------------
uint8_t cardBuffer[NUM_BLOCKS][BLOCK_SIZE];
bool    bufferValid = false;
uint8_t sourceUid[10];
uint8_t sourceUidSize = 0;

// --- Small helpers ---------------------------------------------------------
void printHex(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
    Serial.print(' ');
  }
}

// Block is a sector trailer (block 3, 7, 11, ...). It holds keys/access bits,
// so we never overwrite it blindly.
bool isTrailerBlock(uint8_t block) {
  return (block % 4) == 3;
}

bool detectCard();  // defined below

// Wait up to `timeoutMs` for a card to be selected. Returns true on success.
bool waitForCard(unsigned long timeoutMs = 10000) {
  Serial.println(F("  Present a card on the reader..."));
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (detectCard() && mfrc522.PICC_ReadCardSerial()) {
      return true;
    }
    delay(50);
  }
  Serial.println(F("  Timed out waiting for a card."));
  return false;
}

void haltCard() {
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// Try to wake a card in the field. PICC_IsNewCardPresent() only sends REQA,
// which a halted card ignores, so we also fall back to WUPA. Works for any
// ISO14443-A card (Classic, Ultralight, DESFire, ...) - detection only.
bool detectCard() {
  byte atqa[2];
  byte size = sizeof(atqa);
  // Reset baud rates / modulation width the way the library does before REQA.
  mfrc522.PCD_WriteRegister(MFRC522::TxModeReg, 0x00);
  mfrc522.PCD_WriteRegister(MFRC522::RxModeReg, 0x00);
  mfrc522.PCD_WriteRegister(MFRC522::ModWidthReg, 0x26);
  MFRC522::StatusCode s = mfrc522.PICC_RequestA(atqa, &size);
  if (s != MFRC522::STATUS_OK && s != MFRC522::STATUS_COLLISION) {
    size = sizeof(atqa);
    s = mfrc522.PICC_WakeupA(atqa, &size);  // also wake halted cards
  }
  return (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION);
}

// Authenticate a sector with the default key A.
bool authSector(uint8_t sector) {
  uint8_t trailerBlock = sector * 4 + 3;
  MFRC522::StatusCode status = mfrc522.PCD_Authenticate(
      MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailerBlock, &defaultKey, &(mfrc522.uid));
  return status == MFRC522::STATUS_OK;
}

// --- Commands --------------------------------------------------------------
void printHelp() {
  Serial.println(F("\nrfid-copier commands:"));
  Serial.println(F("  help      - show this help"));
  Serial.println(F("  info      - show UID/type of the presented card"));
  Serial.println(F("  read      - read a card into the buffer (+ hex dump)"));
  Serial.println(F("  dump      - print the current buffer"));
  Serial.println(F("  write     - write buffer data blocks onto a target card"));
  Serial.println(F("  writeuid  - write the read card's UID (magic cards only)"));
  Serial.println(F("  setuid <hex> - write a custom UID, e.g. 'setuid 04 A1 B2 C3'"));
  Serial.println(F("  clear     - discard the buffer"));
  Serial.println();
}

void cmdInfo() {
  if (!waitForCard()) return;
  Serial.print(F("  UID:  "));
  printHex(mfrc522.uid.uidByte, mfrc522.uid.size);
  Serial.println();
  Serial.print(F("  SAK:  0x"));
  Serial.println(mfrc522.uid.sak, HEX);
  MFRC522::PICC_Type type = mfrc522.PICC_GetType(mfrc522.uid.sak);
  Serial.print(F("  Type: "));
  Serial.println(mfrc522.PICC_GetTypeName(type));
  haltCard();
}

void cmdRead() {
  if (!waitForCard()) return;

  // Remember the source UID.
  sourceUidSize = mfrc522.uid.size;
  memcpy(sourceUid, mfrc522.uid.uidByte, sourceUidSize);

  Serial.print(F("  Reading UID: "));
  printHex(sourceUid, sourceUidSize);
  Serial.println();

  uint8_t failedSectors = 0;
  for (uint8_t sector = 0; sector < 16; sector++) {
    if (!authSector(sector)) {
      Serial.print(F("  ! sector "));
      Serial.print(sector);
      Serial.println(F(" auth failed (non-default key?) - zero-filled"));
      failedSectors++;
      for (uint8_t b = 0; b < 4; b++) {
        memset(cardBuffer[sector * 4 + b], 0, BLOCK_SIZE);
      }
      continue;
    }
    for (uint8_t b = 0; b < 4; b++) {
      uint8_t block = sector * 4 + b;
      uint8_t buf[18];           // library needs 2 extra bytes for CRC
      uint8_t len = sizeof(buf);
      MFRC522::StatusCode status = mfrc522.MIFARE_Read(block, buf, &len);
      if (status == MFRC522::STATUS_OK) {
        memcpy(cardBuffer[block], buf, BLOCK_SIZE);
      } else {
        memset(cardBuffer[block], 0, BLOCK_SIZE);
      }
    }
  }

  bufferValid = true;
  haltCard();

  Serial.print(F("  Done. Sectors with auth failures: "));
  Serial.println(failedSectors);
  Serial.println(F("  Buffer now holds the card image. Use 'dump' to view."));
}

void cmdDump() {
  if (!bufferValid) {
    Serial.println(F("  Buffer is empty. Run 'read' first."));
    return;
  }
  Serial.print(F("  Source UID: "));
  printHex(sourceUid, sourceUidSize);
  Serial.println();
  for (uint8_t block = 0; block < NUM_BLOCKS; block++) {
    Serial.print(F("  blk "));
    if (block < 10) Serial.print(' ');
    Serial.print(block);
    Serial.print(isTrailerBlock(block) ? F(" T ") : F("   "));
    printHex(cardBuffer[block], BLOCK_SIZE);
    Serial.println();
  }
}

void cmdWrite() {
  if (!bufferValid) {
    Serial.println(F("  Buffer is empty. Run 'read' first."));
    return;
  }
  if (!waitForCard()) return;

  uint8_t written = 0, failed = 0, skipped = 0;
  for (uint8_t sector = 0; sector < 16; sector++) {
    if (!authSector(sector)) {
      Serial.print(F("  ! sector "));
      Serial.print(sector);
      Serial.println(F(" auth failed - skipping sector"));
      skipped += 4;
      continue;
    }
    for (uint8_t b = 0; b < 4; b++) {
      uint8_t block = sector * 4 + b;
      // Never touch block 0 (manufacturer/UID) or sector trailers here.
      if (block == 0 || isTrailerBlock(block)) {
        skipped++;
        continue;
      }
      MFRC522::StatusCode status = mfrc522.MIFARE_Write(block, cardBuffer[block], BLOCK_SIZE);
      if (status != MFRC522::STATUS_OK) {
        Serial.print(F("  ! write failed at block "));
        Serial.println(block);
        failed++;
        continue;
      }
      // Verify by read-back.
      uint8_t buf[18];
      uint8_t len = sizeof(buf);
      if (mfrc522.MIFARE_Read(block, buf, &len) == MFRC522::STATUS_OK &&
          memcmp(buf, cardBuffer[block], BLOCK_SIZE) == 0) {
        written++;
      } else {
        Serial.print(F("  ! verify mismatch at block "));
        Serial.println(block);
        failed++;
      }
    }
  }
  haltCard();

  Serial.print(F("  Write complete. ok="));
  Serial.print(written);
  Serial.print(F(" failed="));
  Serial.print(failed);
  Serial.print(F(" skipped(block0/trailers)="));
  Serial.println(skipped);
}

// Gen2 / CUID magic cards have no backdoor: block 0 is directly writable after
// a normal authenticate with the default key. Used as a fallback when the
// Gen1a backdoor (MIFARE_SetUid) doesn't respond.
bool writeUidGen2(const uint8_t *newUid, uint8_t len) {
  if (!detectCard() || !mfrc522.PICC_ReadCardSerial()) {
    Serial.println(F("  Gen2: no card selected."));
    return false;
  }
  MFRC522::MIFARE_Key key = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  // Authenticate sector 0 via its trailer block (3).
  if (mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, 3, &key, &mfrc522.uid)
      != MFRC522::STATUS_OK) {
    Serial.println(F("  Gen2: auth of sector 0 failed."));
    return false;
  }
  byte b0[18];
  byte sz = sizeof(b0);
  if (mfrc522.MIFARE_Read(0, b0, &sz) != MFRC522::STATUS_OK) {
    Serial.println(F("  Gen2: read of block 0 failed."));
    mfrc522.PCD_StopCrypto1();
    return false;
  }
  // Overwrite UID bytes + BCC, keep the rest of block 0 (SAK/ATQA/manufacturer).
  byte bcc = 0;
  for (uint8_t i = 0; i < len; i++) { b0[i] = newUid[i]; bcc ^= newUid[i]; }
  b0[len] = bcc;
  MFRC522::StatusCode w = mfrc522.MIFARE_Write(0, b0, 16);
  mfrc522.PCD_StopCrypto1();
  if (w != MFRC522::STATUS_OK) {
    Serial.println(F("  Gen2: write of block 0 failed (not a CUID card?)."));
    return false;
  }
  return true;
}

// Shared core: write `len` UID bytes onto a presented magic card, then
// re-read the card and report the UID it actually presents (ground truth).
void writeUidBytes(const uint8_t *newUid, uint8_t len) {
  if (len != 4 && len != 7) {
    Serial.println(F("  UID must be 4 or 7 bytes."));
    return;
  }
  Serial.print(F("  Target UID: "));
  printHex(newUid, len);
  Serial.println();
  if (len == 7) {
    // The library lays out block 0 as (uid bytes)+(xor BCC), which is the
    // 4-byte scheme. Real 7-byte UIDs use anti-collision cascade bytes, so
    // this only yields a valid UID on a card that genuinely supports a
    // 7-byte writable UID. The read-back below shows what really happened.
    Serial.println(F("  NOTE: 7-byte UID needs a 7-byte-capable magic card."));
    Serial.println(F("  On a 4-byte magic card the result will NOT be valid."));
  }

  if (!waitForCard()) return;

  Serial.println(F("  Writing UID (magic/UID-changeable cards only)..."));
  // First try the Gen1a backdoor; if the card doesn't answer it, try Gen2/CUID.
  bool ok = mfrc522.MIFARE_SetUid((byte *)newUid, len, true);
  if (!ok) {
    Serial.println(F("  Gen1a backdoor failed; trying Gen2 (CUID) direct write..."));
    haltCard();
    delay(50);
    ok = writeUidGen2(newUid, len);
  }
  if (!ok) {
    Serial.println(F("  Write failed - this is not a magic (UID-changeable) card."));
    haltCard();
    return;
  }
  haltCard();

  // Verify by re-reading the UID the card now broadcasts.
  delay(50);
  if (detectCard() && mfrc522.PICC_ReadCardSerial()) {
    Serial.print(F("  Card now presents UID: "));
    printHex(mfrc522.uid.uidByte, mfrc522.uid.size);
    Serial.println();
    bool match = (mfrc522.uid.size == len) &&
                 memcmp(mfrc522.uid.uidByte, newUid, len) == 0;
    Serial.println(match ? F("  MATCH - UID set successfully.")
                         : F("  Mismatch - card may not support this UID length."));
    haltCard();
  } else {
    Serial.println(F("  Wrote, but could not re-read the card to verify."));
  }
}

// Parse a hex string (spaces/colons/dashes ignored) into bytes. Returns count.
uint8_t parseHexBytes(const String &s, uint8_t *out, uint8_t maxLen) {
  uint8_t count = 0;
  int hi = -1;
  for (uint16_t i = 0; i < s.length() && count < maxLen; i++) {
    char c = s[i];
    int v;
    if      (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;  // skip separators
    if (hi < 0) hi = v;
    else { out[count++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return count;
}

// writeuid: write the UID captured by the last `read` onto a magic card.
void cmdWriteUid() {
  if (!bufferValid || sourceUidSize == 0) {
    Serial.println(F("  No source UID. Run 'read' first."));
    return;
  }
  writeUidBytes(sourceUid, sourceUidSize);
}

// setuid <hex>: write an arbitrary UID (e.g. one read off your phone) onto a
// magic card. Accepts 4 or 7 bytes, separators optional.
void cmdSetUid(const String &arg) {
  uint8_t uid[7];
  uint8_t n = parseHexBytes(arg, uid, sizeof(uid));
  if (n != 4 && n != 7) {
    Serial.println(F("  Usage: setuid <hex>   (4 or 7 bytes)"));
    Serial.println(F("  e.g.   setuid 04 A1 B2 C3"));
    Serial.print(F("  Parsed "));
    Serial.print(n);
    Serial.println(F(" byte(s) - need exactly 4 or 7."));
    return;
  }
  writeUidBytes(uid, n);
}

void cmdClear() {
  bufferValid = false;
  sourceUidSize = 0;
  Serial.println(F("  Buffer cleared."));
}

// --- Serial line handling --------------------------------------------------
String line;

void dispatch(const String &input) {
  int sp = input.indexOf(' ');
  String cmd = (sp < 0) ? input : input.substring(0, sp);
  String arg = (sp < 0) ? String("") : input.substring(sp + 1);

  if (cmd == "help" || cmd == "?") printHelp();
  else if (cmd == "info")          cmdInfo();
  else if (cmd == "read")          cmdRead();
  else if (cmd == "dump")          cmdDump();
  else if (cmd == "write")         cmdWrite();
  else if (cmd == "writeuid")      cmdWriteUid();
  else if (cmd == "setuid")        cmdSetUid(arg);
  else if (cmd == "clear")         cmdClear();
  else if (cmd.length() == 0)      { /* ignore blank lines */ }
  else {
    Serial.print(F("  Unknown command: "));
    Serial.println(cmd);
    Serial.println(F("  Type 'help' for the list."));
  }
  Serial.print(F("> "));
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }  // wait for USB serial (harmless on the Nano)
  SPI.begin();
  mfrc522.PCD_Init();
  // Max receiver gain - helps weak/awkward antennas pick up harder cards.
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
  delay(50);

  Serial.println(F("\n=== rfid-copier ready ==="));
  Serial.println(F("RC522 + Arduino Nano. MIFARE Classic 1K."));
  printHelp();
  Serial.print(F("> "));
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) {
        Serial.println(line);   // echo
      }
      dispatch(line);
      line = "";
    } else {
      line += c;
      if (line.length() > 32) line = "";  // guard against runaway input
    }
  }
}
