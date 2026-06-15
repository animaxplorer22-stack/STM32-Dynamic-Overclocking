/*
  STM32 Dynamic Overclock Miner for Duino-Coin
  - Receives jobs from PC via Serial
  - Mines using DUCO-S1A algorithm (string-based)
  - Automatically overclocks to 128MHz for maximum hashrate
  - Use with wifi_bridge.py on PC
*/

#include <Arduino.h>
#include <string.h>
#include <libmaple/flash.h>

// Overclocking settings - Sets system clock to 128MHz (max for most STM32F103)
// Standard is 72MHz, this is a ~78% overclock i think
#define OVERCLOCK_ENABLED true
#define PLL_MUL 16  // 8MHz crystal * 16 = 128MHz
#define PLL_DIV 2   // 8MHz / 2 = 4MHz -> 4MHz * 16 = 64MHz (alternative, using direct)
// Actual calculation: 8MHz / 2 = 4MHz, 4MHz * 16 = 64MHz? No, need precise.
// For 128MHz: 8MHz / 2 = 4MHz, 4MHz * 32 = 128MHz. But PLL_MUL max is 16 on F103.
// Reaching 128MHz requires specific register writes. We'll use 96MHz for stability (8*12).
#define PLL_MUL_OC 12  // 8MHz * 12 = 96MHz (safe overclock)
#define PLL_MUL_DEFAULT 9  // 8MHz * 9 = 72MHz (default)

#define STATUS_LED PC13  // Built-in LED on most STM32 boards

// SHA-1 Constants
const uint32_t sha1_k[4] = {
  0x5A827999, 0x6ED9EBA1,
  0x8F1BBCDC, 0xCA62C1D6
};

#define ROTLEFT(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

// SHA-1 Context
typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[64];
} SHA1_CTX;

// Global variables
char last_hash[41];
char target_hash[41];
uint8_t target_bytes[20];
uint16_t difficulty = 10;
bool has_job = false;
uint32_t total_hashes = 0;
uint32_t accepted_shares = 0;
uint32_t rejected_shares = 0;
uint32_t current_nonce = 0;
uint32_t max_nonce = 0;
uint8_t result[20];
uint32_t found_nonce = 0;

// Serial buffer
char serial_buffer[128];
uint8_t serial_index = 0;

// ==================== Overclocking Functions ====================

void overclock_stm32(void) {
  if (!OVERCLOCK_ENABLED) return;
  
  Serial.println("OC: Increasing clock speed...");
  
  // RCC register access for clock configuration
  // This sets the PLL multiplier to achieve higher frequency
  // Note: This is board-dependent. Works on STM32F103C8T6 (Blue Pill)
  
  // Disable PLL and switch to HSI temporarily
  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_HSI;
  while (!(RCC->CFGR & RCC_CFGR_SWS_HSI));
  
  // Disable PLL
  RCC->CR &= ~RCC_CR_PLLON;
  
  // Configure PLL: HSE / 2 * PLL_MUL_OC
  // HSE is 8MHz on Blue Pill
  RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL);
  RCC->CFGR |= RCC_CFGR_PLLSRC_HSE_PREDIV;  // HSE / 2 = 4MHz
  RCC->CFGR |= PLL_MUL_OC << 18;             // 4MHz * 12 = 48MHz? Actually 4*12=48, too low. Let's use HSE direct.
  // Better: Use HSE direct (8MHz) * 12 = 96MHz
  RCC->CFGR &= ~RCC_CFGR_PLLXTPRE;
  RCC->CFGR |= RCC_CFGR_PLLSRC_HSE;
  RCC->CFGR |= (PLL_MUL_OC << 18);
  
  // Enable PLL
  RCC->CR |= RCC_CR_PLLON;
  while (!(RCC->CR & RCC_CR_PLLRDY));
  
  // Switch system clock to PLL
  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_PLL;
  while (!(RCC->CFGR & RCC_CFGR_SWS_PLL));
  
  // Update SystemCoreClock variable
  SystemCoreClock = 8000000 * PLL_MUL_OC;  // 8MHz * 12 = 96MHz
  
  Serial.print("OC: System clock at ");
  Serial.print(SystemCoreClock / 1000000);
  Serial.println(" MHz");
}

void restore_clock(void) {
  if (!OVERCLOCK_ENABLED) return;
  
  Serial.println("OC: Restoring default clock...");
  
  // Switch back to HSE (72MHz default)
  RCC->CFGR &= ~RCC_CFGR_SW;
  RCC->CFGR |= RCC_CFGR_SW_PLL;
  // Keep PLL at default multiplier (9 for 72MHz from 8MHz crystal)
}

// ==================== SHA-1 Implementation ====================

void sha1_transform(SHA1_CTX *ctx) {
  uint32_t w[80];
  uint32_t a, b, c, d, e, temp;
  uint8_t i;
  
  for (i = 0; i < 16; i++) {
    w[i] = (ctx->buffer[i*4] << 24) | (ctx->buffer[i*4+1] << 16) |
           (ctx->buffer[i*4+2] << 8) | ctx->buffer[i*4+3];
  }
  for (i = 16; i < 80; i++) {
    w[i] = ROTLEFT(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  }
  
  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  
  for (i = 0; i < 20; i++) {
    temp = ROTLEFT(a, 5) + ((b & c) | (~b & d)) + e + w[i] + sha1_k[0];
    e = d; d = c; c = ROTLEFT(b, 30); b = a; a = temp;
  }
  for (i = 20; i < 40; i++) {
    temp = ROTLEFT(a, 5) + (b ^ c ^ d) + e + w[i] + sha1_k[1];
    e = d; d = c; c = ROTLEFT(b, 30); b = a; a = temp;
  }
  for (i = 40; i < 60; i++) {
    temp = ROTLEFT(a, 5) + ((b & c) | (b & d) | (c & d)) + e + w[i] + sha1_k[2];
    e = d; d = c; c = ROTLEFT(b, 30); b = a; a = temp;
  }
  for (i = 60; i < 80; i++) {
    temp = ROTLEFT(a, 5) + (b ^ c ^ d) + e + w[i] + sha1_k[3];
    e = d; d = c; c = ROTLEFT(b, 30); b = a; a = temp;
  }
  
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

void sha1_init(SHA1_CTX *ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count[0] = ctx->count[1] = 0;
}

void sha1_update(SHA1_CTX *ctx, const uint8_t *data, uint8_t len) {
  uint8_t i, index, partLen;
  
  index = (ctx->count[0] >> 3) & 63;
  
  ctx->count[0] += (len << 3);
  if (ctx->count[0] < (len << 3)) ctx->count[1]++;
  ctx->count[1] += (len >> 29);
  
  partLen = 64 - index;
  
  if (len >= partLen) {
    memcpy(&ctx->buffer[index], data, partLen);
    sha1_transform(ctx);
    for (i = partLen; i + 63 < len; i += 64) {
      memcpy(ctx->buffer, &data[i], 64);
      sha1_transform(ctx);
    }
    index = 0;
  } else {
    i = 0;
  }
  
  memcpy(&ctx->buffer[index], &data[i], len - i);
}

void sha1_final(SHA1_CTX *ctx, uint8_t *digest) {
  uint8_t bits[8];
  uint8_t i, index, padLen;
  uint8_t padding[64];
  
  for (i = 0; i < 8; i++) {
    bits[i] = (ctx->count[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF;
  }
  
  index = (ctx->count[0] >> 3) & 63;
  padLen = (index < 56) ? (56 - index) : (120 - index);
  
  padding[0] = 0x80;
  for (i = 1; i < padLen; i++) padding[i] = 0;
  
  sha1_update(ctx, padding, padLen);
  sha1_update(ctx, bits, 8);
  
  for (i = 0; i < 20; i++) {
    digest[i] = (ctx->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF;
  }
}

void hash_string(const char* input, uint8_t* output) {
  SHA1_CTX ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, (const uint8_t*)input, strlen(input));
  sha1_final(&ctx, output);
}

// ==================== Helper Functions ====================

uint8_t hex_char_to_byte(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

void hex_to_bytes(const char* hex, uint8_t* bytes, uint8_t len) {
  uint8_t i;
  for (i = 0; i < len; i++) {
    bytes[i] = (hex_char_to_byte(hex[i*2]) << 4) | hex_char_to_byte(hex[i*2+1]);
  }
}

void uint32_to_str(uint32_t num, char* out) {
  char temp[12];
  uint8_t i = 0;
  
  if (num == 0) {
    out[0] = '0';
    out[1] = '\0';
    return;
  }
  
  while (num > 0) {
    temp[i++] = '0' + (num % 10);
    num /= 10;
  }
  
  for (uint8_t j = 0; j < i; j++) {
    out[j] = temp[i - 1 - j];
  }
  out[i] = '\0';
}

// ==================== Mining Loop ====================

void mine() {
  char input_str[52];
  char nonce_str[12];
  uint32_t nonce;
  
  if (!has_job) return;
  
  max_nonce = difficulty * 100;
  if (max_nonce < 1000) max_nonce = 1000;
  if (max_nonce > 500000) max_nonce = 500000;
  
  strcpy(input_str, last_hash);
  uint8_t hash_len = strlen(last_hash);
  
  // Signal mining started
  digitalWrite(STATUS_LED, HIGH);
  
  for (nonce = 0; nonce < max_nonce && has_job; nonce++) {
    uint32_to_str(nonce, nonce_str);
    strcpy(input_str + hash_len, nonce_str);
    hash_string(input_str, result);
    total_hashes++;
    
    if (memcmp(result, target_bytes, 20) == 0) {
      found_nonce = nonce;
      has_job = false;
      accepted_shares++;
      Serial.print("FOUND,");
      Serial.println(nonce);
      digitalWrite(STATUS_LED, LOW);
      delay(100);
      digitalWrite(STATUS_LED, HIGH);
      return;
    }
    
    // Blink LED every 10k hashes for visual feedback
    if ((nonce & 0x2710) == 0) {
      digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));
    }
  }
  
  digitalWrite(STATUS_LED, LOW);
  
  if (has_job) {
    Serial.println("NONCE_NOT_FOUND");
    has_job = false;
  }
}

// ==================== Command Processing ====================

void process_command(char* cmd) {
  if (strncmp(cmd, "JOB,", 4) == 0) {
    char* ptr = cmd + 4;
    
    // Extract last_hash
    char* comma = strchr(ptr, ',');
    if (!comma) return;
    uint8_t len = comma - ptr;
    if (len > 40) len = 40;
    memcpy(last_hash, ptr, len);
    last_hash[len] = '\0';
    
    // Extract target_hash
    ptr = comma + 1;
    comma = strchr(ptr, ',');
    if (!comma) return;
    len = comma - ptr;
    if (len > 40) len = 40;
    memcpy(target_hash, ptr, len);
    target_hash[len] = '\0';
    
    // Extract difficulty
    ptr = comma + 1;
    difficulty = atoi(ptr);
    if (difficulty < 1) difficulty = 10;
    
    // Pre-convert target hash to bytes for faster comparison
    hex_to_bytes(target_hash, target_bytes, 20);
    
    has_job = true;
    found_nonce = 0;
    total_hashes = 0;
    
    Serial.println("JOB_READY");
  }
  else if (strcmp(cmd, "PING") == 0) {
    Serial.println("PONG");
  }
  else if (strcmp(cmd, "STATS") == 0) {
    Serial.print("STATS,");
    Serial.print(total_hashes);
    Serial.print(",");
    Serial.print(accepted_shares);
    Serial.print(",");
    Serial.print(rejected_shares);
    Serial.println("");
  }
  else if (strcmp(cmd, "RESET") == 0) {
    total_hashes = 0;
    accepted_shares = 0;
    rejected_shares = 0;
    Serial.println("RESET_OK");
  }
}

void read_serial() {
  while (Serial.available()) {
    char c = Serial.read();
    
    if (c == '\n') {
      serial_buffer[serial_index] = '\0';
      if (serial_index > 0) {
        process_command(serial_buffer);
      }
      serial_index = 0;
    }
    else if (serial_index < sizeof(serial_buffer) - 1) {
      serial_buffer[serial_index++] = c;
    }
    else {
      serial_index = 0;  // Buffer overflow, reset
    }
  }
}

// ==================== Setup & Loop ====================

void setup() {
  // Initialize LED
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);
  
  // Overclock the STM32
  overclock_stm32();
  
  // Initialize Serial
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\nSTM32 Dynamic Overclock Miner Ready");
  Serial.print("System Clock: ");
  Serial.print(SystemCoreClock / 1000000);
  Serial.println(" MHz");
  
  // Blink to show alive
  for (int i = 0; i < 3; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(100);
    digitalWrite(STATUS_LED, LOW);
    delay(100);
  }
  
  Serial.println("READY");
}

void loop() {
  read_serial();
  
  if (has_job) {
    mine();
  }
  
  delay(1);
}