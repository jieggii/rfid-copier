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

// Wait up to `timeoutMs` for a card to be selected. Returns true on success.
bool waitForCard(unsigned long timeoutMs = 10000) {
  Serial.println(F("  Present a card on the reader..."));
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
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
  Serial.println(F("  writeuid  - attempt to write the UID (magic cards only)"));
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

void cmdWriteUid() {
  if (!bufferValid) {
    Serial.println(F("  Buffer is empty. Run 'read' first."));
    return;
  }
  if (sourceUidSize != 4) {
    Serial.println(F("  UID write only supported for 4-byte UIDs."));
    return;
  }
  if (!waitForCard()) return;

  Serial.println(F("  Attempting UID write (works only on 'magic' cards)..."));
  // MIFARE_SetUid writes the stored source UID onto the presented card.
  if (mfrc522.MIFARE_SetUid(sourceUid, sourceUidSize, true)) {
    Serial.println(F("  UID write OK (this is a magic card)."));
  } else {
    Serial.println(F("  UID write failed - normal/genuine card (expected)."));
  }
  haltCard();
}

void cmdClear() {
  bufferValid = false;
  sourceUidSize = 0;
  Serial.println(F("  Buffer cleared."));
}

// --- Serial line handling --------------------------------------------------
String line;

void dispatch(const String &cmd) {
  if (cmd == "help" || cmd == "?") printHelp();
  else if (cmd == "info")          cmdInfo();
  else if (cmd == "read")          cmdRead();
  else if (cmd == "dump")          cmdDump();
  else if (cmd == "write")         cmdWrite();
  else if (cmd == "writeuid")      cmdWriteUid();
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
