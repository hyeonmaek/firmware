// 방송 보안(secure.h)을 맥에서 검증한다. 보드에 올리는 바로 그 코드를 쓴다.
// 1) AES-128을 NIST 공식 시험값(FIPS-197)으로 정확성 증명
// 2) 암호화 왕복(내 보드 → 동료 보드)
// 3) 공격 시연: 위조(열쇠 없음) 거부 / 재생(옛 방송) 거부
// 빌드·실행: cc -I../Tap_Talk -o /tmp/crypto_test crypto_test.c && /tmp/crypto_test
#include <stdio.h>
#include <string.h>
#include "secure.h"

static int fails = 0;
static void check(const char* name, int ok) {
  printf("%s %s\n", ok ? "  통과" : "  실패 ❌", name);
  if (!ok) fails++;
}

int main(void) {
  // 1) FIPS-197 부록 B 공식 시험값
  uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
  uint8_t pt[16]  = {0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34};
  uint8_t want[16]= {0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32};
  Aes128 a; aes128_init(&a, key);
  uint8_t ct[16]; aes128_encrypt_block(&a, pt, ct);
  check("AES-128 NIST 시험값 일치", memcmp(ct, want, 16) == 0);

  // FIPS-197 부록 C.1 (또 하나의 공식 시험값)
  uint8_t k2[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  uint8_t p2[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  uint8_t w2[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a};
  aes128_init(&a, k2); aes128_encrypt_block(&a, p2, ct);
  check("AES-128 NIST 시험값 2 일치", memcmp(ct, w2, 16) == 0);

  // 2) 암호화 왕복: E-2가 "적 발견·사격 개시, 방향 215도"를 방송 → E-1이 받는다
  SecCtx s; sec_init(&s, SEC_MASTER_KEY);
  SecReplay rx; sec_replay_reset(&rx);
  uint8_t content[SEC_CONTENT] = {3 /*ENEMY_FIRE*/, 1 /*이벤트순번*/, 73 /*배터리%*/, 215 & 0xFF, 215 >> 8, 0 /*ack*/,
    0x40,0x2E,0x5A,0x16, /*위도 37.5665E7 LE*/ 0x30,0x6C,0x15,0x4B /*경도*/};
  uint8_t frame100[SEC_FRAME], frame[SEC_FRAME];
  sec_seal(&s, 0xFFFF, 2 /*E-2*/, 100 /*카운터*/, content, frame100);
  memcpy(frame, frame100, SEC_FRAME);

  uint8_t gotId; uint32_t gotCtr; uint8_t got[SEC_CONTENT];
  int r = sec_open(&s, &rx, 0xFFFF, frame, SEC_FRAME, &gotId, &gotCtr, got);
  check("정상 방송 열림(OK)", r == SEC_OK);
  check("내용 그대로 복원", memcmp(got, content, SEC_CONTENT) == 0 && gotId == 2 && gotCtr == 100);

  // 암호문에 평문이 안 보인다: 방향 215나 이벤트 3이 프레임에 그대로 없음
  printf("  프레임(공개): ");
  for (int i = 0; i < SEC_FRAME; i++) printf("%02X ", frame[i]);
  printf("\n  → 7~20바이트가 내용(GPS 포함) 암호문. nRF Connect로 봐도 무작위 바이트.\n");

  // 3-a) 위조 공격: 열쇠 없는 공격자가 내용을 'OPEN FIRE'로 꾸며 태그를 아무 값이나 붙임
  uint8_t forged[SEC_FRAME];
  memcpy(forged, frame, SEC_FRAME);
  forged[3] = 7;          // 가짜 보낸이
  forged[4] = 200;        // 다른 카운터
  forged[7] ^= 0xAB;      // 내용 조작
  // 태그는 열쇠가 있어야 맞출 수 있음 → 그대로 두면 불일치
  r = sec_open(&s, &rx, 0xFFFF, forged, SEC_FRAME, &gotId, &gotCtr, got);
  check("위조 방송 거부(FORGED)", r == SEC_FORGED);

  // 공격자가 태그 4바이트를 전부 뒤져도(2^32분의 1) 맞출 확률은 사실상 0 — 한 번 더 확인
  forged[12] ^= 0xFF; forged[13] ^= 0x01;
  r = sec_open(&s, &rx, 0xFFFF, forged, SEC_FRAME, &gotId, &gotCtr, got);
  check("위조 태그 바꿔도 거부", r == SEC_FORGED);

  // 같은 방송이 정상적으로 또 들어오면(카운터 100 그대로) 조용히 무시(DUP) — 경보 아님
  r = sec_open(&s, &rx, 0xFFFF, frame100, SEC_FRAME, &gotId, &gotCtr, got);
  check("같은 카운터 반복은 무시(DUP)", r == SEC_DUP);

  // 다음 진짜 방송(카운터 101)은 정상 통과
  sec_seal(&s, 0xFFFF, 2, 101, content, frame);
  r = sec_open(&s, &rx, 0xFFFF, frame, SEC_FRAME, &gotId, &gotCtr, got);
  check("그 다음 진짜 방송(카운터 101) 통과", r == SEC_OK);

  // 3-b) 재생 공격: 공격자가 아까 녹음해 둔 옛 방송(카운터 100)을 지금 다시 틀기
  r = sec_open(&s, &rx, 0xFFFF, frame100, SEC_FRAME, &gotId, &gotCtr, got);
  check("옛 방송 재생 거부(REPLAY)", r == SEC_REPLAY);

  printf(fails ? "\n실패 %d개 ❌\n" : "\n전부 통과 ✅\n", fails);
  return fails ? 1 : 0;
}
