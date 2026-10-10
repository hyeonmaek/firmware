// secure.h — 분대 방송 보안 (AES-128 암호화 + 인증 태그 + 재생 방지 카운터)
//
// 왜 필요한가: 블루투스 방송은 누구나 들을 수 있다(확성기). 그래서
//  1) 내용을 AES-128로 암호화해 도청해도 못 읽게 하고(기밀),
//  2) 인증 태그(CBC-MAC)로 열쇠 없는 가짜 방송을 거르고(위조 차단),
//  3) 카운터로 예전 방송을 다시 틀어도 거르게 한다(재생 차단).
//
// 이 파일은 아두이노(Tap_Talk.ino)와 맥 테스트(tools/crypto_test.c)가 똑같이 포함한다.
// 맥에서 NIST 공식 시험값(FIPS-197)으로 정확성을 증명한 바로 그 코드가 보드에 올라간다.
// 아두이노 의존성 없음(순수 C). 실전에서는 MASTER_KEY를 폰이 QR로 장전한다(지금은 데모 고정키).
#pragma once
#include <stdint.h>
#include <string.h>

// ---------------- AES-128 (FIPS-197) ----------------
// 작고 검증 가능한 구현. 라운드키는 처음 한 번만 펼친다.
typedef struct { uint8_t rk[176]; } Aes128;

static const uint8_t AES_SBOX[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};

static inline uint8_t aes_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aes128_init(Aes128* a, const uint8_t key[16]) {
  static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
  memcpy(a->rk, key, 16);
  for (int i = 4; i < 44; i++) {
    uint8_t t[4];
    memcpy(t, a->rk + (i - 1) * 4, 4);
    if (i % 4 == 0) {
      uint8_t tmp = t[0];
      t[0] = (uint8_t)(AES_SBOX[t[1]] ^ rcon[i / 4 - 1]);
      t[1] = AES_SBOX[t[2]];
      t[2] = AES_SBOX[t[3]];
      t[3] = AES_SBOX[tmp];
    }
    for (int j = 0; j < 4; j++) a->rk[i * 4 + j] = a->rk[(i - 4) * 4 + j] ^ t[j];
  }
}

// 블록 한 개(16바이트) 암호화 (ECB)
static void aes128_encrypt_block(const Aes128* a, const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  for (int i = 0; i < 16; i++) s[i] ^= a->rk[i];
  for (int round = 1; round <= 10; round++) {
    for (int i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];           // SubBytes
    uint8_t t;                                                     // ShiftRows
    t = s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13]; s[13]=t;
    t = s[2];  s[2]=s[10]; s[10]=t;    t=s[6];     s[6]=s[14]; s[14]=t;
    t = s[15]; s[15]=s[11];s[11]=s[7]; s[7]=s[3];  s[3]=t;
    if (round != 10) {                                             // MixColumns
      for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t h = a0 ^ a1 ^ a2 ^ a3;
        col[0] ^= h ^ aes_xtime(a0 ^ a1);
        col[1] ^= h ^ aes_xtime(a1 ^ a2);
        col[2] ^= h ^ aes_xtime(a2 ^ a3);
        col[3] ^= h ^ aes_xtime(a3 ^ a0);
      }
    }
    for (int i = 0; i < 16; i++) s[i] ^= a->rk[round * 16 + i];    // AddRoundKey
  }
  memcpy(out, s, 16);
}

// ---------------- 방송 프레임 ----------------
// 평문 내용(6바이트): [이벤트][이벤트순번][배터리][방향 lo][방향 hi][확인번호]
//   - 이벤트순번: 같은 이벤트를 여러 번 처리하지 않게 하는 번호(암호 카운터와 별개).
//     암호 카운터는 배터리만 바뀌어도 매번 증가(키스트림 재사용 금지)하지만,
//     이벤트순번은 새 사건일 때만 바뀌므로 수신자가 사건을 한 번만 처리한다.
// 프레임(17바이트): [회사ID 2][버전 1][보낸이ID 1][카운터 3][암호문 6][태그 4]
//   - 회사ID·버전·보낸이·카운터는 평문(스캔 필터·논스·수신자 구분용. 내용은 전부 암호문)
#define SEC_VER        0x02
#define SEC_CONTENT    14   // [이벤트][순번][배터리][방향lo][방향hi][확인번호][위도E7 4][경도E7 4]
#define SEC_TAG        4
#define SEC_FRAME      (2 + 1 + 1 + 3 + SEC_CONTENT + SEC_TAG)   // = 25 (광고 26바이트 한도 안)

// 분대 공용 열쇠 SEC_MASTER_KEY[16]는 squad_key.h에(git에 올리지 않음, squad_key.h.example 참고).
// 실전: 폰이 QR로 장전. K_enc·K_mac는 여기서 파생해 용도를 분리한다.
#include "squad_key.h"

typedef struct { Aes128 enc, mac; int ready; } SecCtx;

static void sec_init(SecCtx* s, const uint8_t master[16]) {
  Aes128 km; aes128_init(&km, master);
  uint8_t blk[16], out[16];
  memset(blk, 0, 16); blk[0] = 0x01; aes128_encrypt_block(&km, blk, out); aes128_init(&s->enc, out);
  memset(blk, 0, 16); blk[0] = 0x02; aes128_encrypt_block(&km, blk, out); aes128_init(&s->mac, out);
  s->ready = 1;
}

// CTR 키스트림 한 블록: 논스 = [버전, 보낸이, 카운터3, 0...], 내용≤16이라 블록 하나면 충분
static void sec_keystream(const SecCtx* s, uint8_t ver, uint8_t id, const uint8_t ctr[3], uint8_t ks[16]) {
  uint8_t nonce[16]; memset(nonce, 0, 16);
  nonce[0] = ver; nonce[1] = id; nonce[2] = ctr[0]; nonce[3] = ctr[1]; nonce[4] = ctr[2];
  aes128_encrypt_block(&s->enc, nonce, ks);
}

// 인증 태그: CBC-MAC over [버전,보낸이,카운터3,암호문] + 0x80 패딩(여러 블록) → 앞 4바이트
static void sec_tag(const SecCtx* s, uint8_t ver, uint8_t id, const uint8_t ctr[3],
                    const uint8_t ct[SEC_CONTENT], uint8_t tag[SEC_TAG]) {
  uint8_t ad[48]; memset(ad, 0, sizeof(ad));
  ad[0] = ver; ad[1] = id; ad[2] = ctr[0]; ad[3] = ctr[1]; ad[4] = ctr[2];
  memcpy(ad + 5, ct, SEC_CONTENT);
  int total = 5 + SEC_CONTENT;
  ad[total] = 0x80;                                  // 패딩
  int nblk = (total + 1 + 15) / 16;
  uint8_t x[16]; memset(x, 0, 16);
  for (int b = 0; b < nblk; b++) {                   // CBC 체인
    uint8_t t[16];
    for (int i = 0; i < 16; i++) t[i] = ad[b * 16 + i] ^ x[i];
    aes128_encrypt_block(&s->mac, t, x);
  }
  memcpy(tag, x, SEC_TAG);
}

// 내용(5바이트)+카운터 → 프레임(16바이트). 반환: SEC_FRAME
static int sec_seal(const SecCtx* s, uint16_t company, uint8_t id, uint32_t counter,
                    const uint8_t content[SEC_CONTENT], uint8_t frame[SEC_FRAME]) {
  uint8_t ctr[3] = {(uint8_t)(counter & 0xFF), (uint8_t)((counter >> 8) & 0xFF), (uint8_t)((counter >> 16) & 0xFF)};
  uint8_t ks[16]; sec_keystream(s, SEC_VER, id, ctr, ks);
  uint8_t ct[SEC_CONTENT];
  for (int i = 0; i < SEC_CONTENT; i++) ct[i] = content[i] ^ ks[i];
  uint8_t tag[SEC_TAG]; sec_tag(s, SEC_VER, id, ctr, ct, tag);
  frame[0] = (uint8_t)(company & 0xFF); frame[1] = (uint8_t)(company >> 8);
  frame[2] = SEC_VER; frame[3] = id;
  frame[4] = ctr[0]; frame[5] = ctr[1]; frame[6] = ctr[2];
  memcpy(frame + 7, ct, SEC_CONTENT);
  memcpy(frame + 7 + SEC_CONTENT, tag, SEC_TAG);
  return SEC_FRAME;
}

// 열어 보기 결과
#define SEC_OK        0
#define SEC_BAD_FMT   1   // 우리 프레임 아님
#define SEC_FORGED    2   // 태그 불일치(열쇠 없는 위조)
#define SEC_REPLAY    3   // 카운터가 더 작음(예전 방송을 녹음해 다시 틀기)
#define SEC_DUP       4   // 카운터가 같음(방금 그 방송의 정상 반복 — 조용히 무시)

// 재생 방지: 보낸이별 마지막 카운터 기억
#define SEC_PEERS 8
typedef struct { uint8_t id; uint32_t lastCtr; int used; } SecPeer;
typedef struct { SecPeer p[SEC_PEERS]; } SecReplay;
static void sec_replay_reset(SecReplay* r) { memset(r, 0, sizeof(*r)); }

// 프레임 → 내용. verify 성공 시 content 채우고 SEC_OK. replay가 NULL이면 카운터 검사 생략.
static int sec_open(const SecCtx* s, SecReplay* replay, uint16_t company,
                    const uint8_t* frame, int len, uint8_t* outId, uint32_t* outCtr,
                    uint8_t content[SEC_CONTENT]) {
  if (len < SEC_FRAME) return SEC_BAD_FMT;
  if (frame[0] != (uint8_t)(company & 0xFF) || frame[1] != (uint8_t)(company >> 8)) return SEC_BAD_FMT;
  if (frame[2] != SEC_VER) return SEC_BAD_FMT;
  uint8_t id = frame[3];
  uint8_t ctr[3] = {frame[4], frame[5], frame[6]};
  const uint8_t* ct = frame + 7;
  const uint8_t* tag = frame + 7 + SEC_CONTENT;
  uint8_t want[SEC_TAG]; sec_tag(s, SEC_VER, id, ctr, ct, want);
  uint8_t diff = 0;                                  // 상수 시간 비교
  for (int i = 0; i < SEC_TAG; i++) diff |= want[i] ^ tag[i];
  if (diff) return SEC_FORGED;
  uint32_t counter = (uint32_t)ctr[0] | ((uint32_t)ctr[1] << 8) | ((uint32_t)ctr[2] << 16);
  if (replay) {
    SecPeer* slot = 0; SecPeer* freeSlot = 0;
    for (int i = 0; i < SEC_PEERS; i++) {
      if (replay->p[i].used && replay->p[i].id == id) { slot = &replay->p[i]; break; }
      if (!replay->p[i].used && !freeSlot) freeSlot = &replay->p[i];
    }
    if (slot) {
      if (counter < slot->lastCtr) return SEC_REPLAY;     // 더 작은 카운터 = 예전 방송 재생
      if (counter == slot->lastCtr) return SEC_DUP;        // 같은 카운터 = 정상 반복 방송
      slot->lastCtr = counter;
    } else if (freeSlot) {
      freeSlot->used = 1; freeSlot->id = id; freeSlot->lastCtr = counter;  // 처음 본 대원
    }
  }
  uint8_t ks[16]; sec_keystream(s, SEC_VER, id, ctr, ks);
  for (int i = 0; i < SEC_CONTENT; i++) content[i] = ct[i] ^ ks[i];
  if (outId) *outId = id;
  if (outCtr) *outCtr = counter;
  return SEC_OK;
}
