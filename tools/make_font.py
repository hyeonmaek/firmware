#!/usr/bin/env python3
"""BDF 픽셀 글꼴에서 필요한 글자만 골라 대원 보드(NU-40)용 C 헤더로 변환.

사용법: python3 tools/make_font.py <Galmuri11.bdf> Tap_Talk/hangul_font.h

고르는 글자: 영문·숫자·기호(ASCII), 한글 자모(ㄱ~ㅣ), KS X 1001 완성형 한글 2,350자, 자주 쓰는 기호.
글꼴: Galmuri (SIL Open Font License 1.1, https://github.com/quiple/galmuri)
"""
import sys


def wanted_codepoints():
    cps = set(range(0x20, 0x7F))                       # ASCII
    cps |= set(range(0x3131, 0x3164))                  # 한글 호환 자모 ㄱ~ㅣ
    for hi in range(0xB0, 0xC9):                       # KS X 1001 완성형 한글 2,350자
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("euc-kr")
            except UnicodeDecodeError:
                continue
            cps.add(ord(ch))
    cps |= {ord(c) for c in "·…“”‘’→←↑↓℃※○●■□▲△▼▽×°「」『』【】〈〉《》、。！？"}
    return cps


def parse_bdf(path):
    glyphs = {}
    ascent = descent = None
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        if line.startswith("FONT_ASCENT"):
            ascent = int(line.split()[1])
        elif line.startswith("FONT_DESCENT"):
            descent = int(line.split()[1])
        elif line.startswith("STARTCHAR"):
            enc = dwidth = None
            bbx = None
            rows = []
            for line in lines:
                if line.startswith("ENCODING"):
                    enc = int(line.split()[1])
                elif line.startswith("DWIDTH"):
                    dwidth = int(line.split()[1])
                elif line.startswith("BBX"):
                    bbx = tuple(int(v) for v in line.split()[1:5])
                elif line == "BITMAP":
                    for line in lines:
                        if line == "ENDCHAR":
                            break
                        rows.append(line.strip())
                    break
            if enc is not None and bbx is not None:
                glyphs[enc] = (dwidth, bbx, rows)
    return glyphs, ascent, descent


def main():
    src, dst = sys.argv[1], sys.argv[2]
    glyphs, ascent, descent = parse_bdf(src)
    codes = sorted(cp for cp in wanted_codepoints() if cp in glyphs)

    bitmap = bytearray()
    meta = []
    for cp in codes:
        dwidth, (w, h, xoff, yoff), rows = glyphs[cp]
        offset = len(bitmap)
        row_bytes = (w + 7) // 8
        for r in rows[:h]:
            val = bytes.fromhex(r.ljust(row_bytes * 2, "0")[: row_bytes * 2])
            bitmap += val
        meta.append((offset, dwidth, w, h, xoff, yoff))

    with open(dst, "w") as out:
        out.write("// 자동 생성: tools/make_font.py — 직접 고치지 말 것\n")
        out.write("// 글꼴: Galmuri11 (Copyright (c) 2019–2025 Lee Minseo, SIL Open Font License 1.1)\n")
        out.write("// 글자: ASCII, 한글 자모, KS X 1001 완성형 한글 2,350자, 기호 일부\n")
        out.write("#pragma once\n#include <stdint.h>\n\n")
        out.write(f"#define HFONT_COUNT {len(codes)}\n")
        out.write(f"#define HFONT_ASCENT {ascent}\n#define HFONT_DESCENT {descent}\n\n")
        out.write("struct HGlyph { uint32_t offset; uint8_t dwidth, w, h; int8_t xoff, yoff; };\n\n")
        out.write("static const uint16_t HFONT_CODES[HFONT_COUNT] = {\n")
        for i in range(0, len(codes), 16):
            out.write("  " + ", ".join(f"0x{c:04X}" for c in codes[i:i + 16]) + ",\n")
        out.write("};\n\n")
        out.write("static const HGlyph HFONT_GLYPHS[HFONT_COUNT] = {\n")
        for m in meta:
            out.write("  {%d, %d, %d, %d, %d, %d},\n" % m)
        out.write("};\n\n")
        out.write(f"static const uint8_t HFONT_BITMAP[{len(bitmap)}] = {{\n")
        for i in range(0, len(bitmap), 24):
            out.write("  " + ", ".join(f"0x{b:02X}" for b in bitmap[i:i + 24]) + ",\n")
        out.write("};\n")
    print(f"글자 {len(codes)}개, 그림 {len(bitmap)}바이트, 정보 {len(codes) * 10}바이트 → {dst}")


if __name__ == "__main__":
    main()
