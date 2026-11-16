#ifndef SS_IMAGE_FORMAT_H
#define SS_IMAGE_FORMAT_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct ss_image_segment {
  uint32_t address, memory, offset, bytes, protection;
};
struct ss_image {
  uint32_t low, high, entry;
  unsigned count;
  struct ss_image_segment segments[64];
  /* Private: format 0 = ELF, 1 = Mach-O; file offset and size of fixups.
     Mach-O retains every segment index; PAGEZERO/LINKEDIT have zero memory. */
  uint32_t format, reloc_offset, reloc_bytes;
};
static int ss_img_error(char *e, size_t n, const char *s) {
  if (n)
    snprintf(e, n, "%s", s);
  return 0;
}
static uint32_t ss_img_u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static int ss_img_range(size_t n, uint32_t a, uint32_t b) {
  return a <= n && b <= n - a;
}
static int ss_img_target(const struct ss_image *i, uint32_t a, uint32_t n) {
  unsigned k;
  for (k = 0; k < i->count; k++) {
    const struct ss_image_segment *s = i->segments + k;
    if (n <= s->memory && a >= s->address && a - s->address <= s->memory - n)
      return 1;
  }
  return 0;
}
static int ss_img_fileva(const struct ss_image *i, uint32_t a, uint32_t n,
                         uint32_t *off) {
  unsigned k;
  for (k = 0; k < i->count; k++) {
    const struct ss_image_segment *s = i->segments + k;
    if (n <= s->bytes && a >= s->address && a - s->address <= s->bytes - n) {
      *off = s->offset + a - s->address;
      return 1;
    }
  }
  return 0;
}
static int ss_img_segment(struct ss_image *i, uint32_t a, uint32_t m,
                          uint32_t o, uint32_t b, uint32_t p, size_t size) {
  unsigned k;
  struct ss_image_segment *s;
  if (i->count == 64 || b > m || !ss_img_range(size, o, b) ||
      (uint64_t)a + m > UINT32_MAX)
    return 0;
  for (k = 0; k < i->count; k++) {
    s = i->segments + k;
    if (m && s->memory && (uint64_t)a + m > s->address &&
        (uint64_t)s->address + s->memory > a)
      return 0;
  }
  s = i->segments + i->count++;
  s->address = a;
  s->memory = m;
  s->offset = o;
  s->bytes = b;
  s->protection = p;
  if (m) {
    if (a < i->low)
      i->low = a;
    if (a + m > i->high)
      i->high = a + m;
  }
  return 1;
}
static int ss_image_parse(const uint8_t *f, size_t z, int macho,
                          struct ss_image *i, char *e, size_t en) {
  uint32_t k, o, n, step, doff = 0, dsize = 0, text = 0, entryoff = 0;
  int main_seen = 0, dyld_seen = 0;
  memset(i, 0, sizeof *i);
  i->low = UINT32_MAX;
  i->format = macho ? 1 : 0;
#define SS_FAIL(s) return ss_img_error(e, en, s)
  if (!macho) {
    if (z < 52 || memcmp(f, "\177ELF", 4) || f[4] != 1 || f[5] != 1 ||
        f[6] != 1 || f[16] != 3 || f[17] || f[18] != 62 || f[19] ||
        ss_img_u32(f + 20) != 1)
      SS_FAIL("expected little-endian x32 static PIE");
    i->entry = ss_img_u32(f + 24);
    o = ss_img_u32(f + 28);
    step = f[42] | f[43] << 8;
    n = f[44] | f[45] << 8;
    if (step != 32 || n > 128 || !ss_img_range(z, o, n * step))
      SS_FAIL("invalid program headers");
    for (k = 0; k < n; k++) {
      const uint8_t *p = f + o + k * step;
      uint32_t t = ss_img_u32(p), flags = ss_img_u32(p + 24);
      if (t == 1) {
        if (!ss_img_segment(
                i, ss_img_u32(p + 8), ss_img_u32(p + 20), ss_img_u32(p + 4),
                ss_img_u32(p + 16),
                ((flags & 4) ? 1 : 0) | (flags & 2) | ((flags & 1) ? 4 : 0), z))
          SS_FAIL("invalid load segment");
      } else if (t == 2) {
        if (dsize)
          SS_FAIL("duplicate dynamic segment");
        doff = ss_img_u32(p + 4);
        dsize = ss_img_u32(p + 16);
      } else if (t == 3)
        SS_FAIL("dynamic interpreter unsupported");
    }
    if (dsize) {
      uint32_t ra = 0, rs = 0, re = 12;
      int done = 0;
      if (dsize % 8 || !ss_img_range(z, doff, dsize))
        SS_FAIL("invalid dynamic table");
      for (k = 0; k < dsize; k += 8) {
        uint32_t t = ss_img_u32(f + doff + k), v = ss_img_u32(f + doff + k + 4);
        if (!t) {
          done = 1;
          break;
        }
        if (t == 7)
          ra = v;
        else if (t == 8)
          rs = v;
        else if (t == 9)
          re = v;
        else if (t == 1 || t == 17 || t == 18 || t == 23 || t == 2 || t == 35 ||
                 t == 36 || t == 37)
          SS_FAIL("unsupported dynamic binding");
      }
      if (!done || re != 12 || rs % 12 ||
          (rs && !ss_img_fileva(i, ra, rs, &i->reloc_offset)))
        SS_FAIL("invalid RELA table");
      i->reloc_bytes = rs;
    }
  } else {
    if (z < 28 || ss_img_u32(f) != 0xfeedface ||
        ss_img_u32(f + 4) != 0x0200000c || ss_img_u32(f + 12) != 2 ||
        !(ss_img_u32(f + 24) & 0x200000))
      SS_FAIL("expected arm64_32 PIE Mach-O");
    n = ss_img_u32(f + 16);
    dsize = ss_img_u32(f + 20);
    o = 28;
    if (n > 256 || !ss_img_range(z, o, dsize))
      SS_FAIL("invalid load commands");
    doff = o + dsize;
    for (k = 0; k < n; k++) {
      uint32_t t, c;
      if (!ss_img_range(doff, o, 8))
        SS_FAIL("truncated command");
      t = ss_img_u32(f + o);
      c = ss_img_u32(f + o + 4);
      if (c < 8 || !ss_img_range(doff, o, c))
        SS_FAIL("invalid command size");
      if (t == 1) {
        uint32_t a, m, b, p;
        if (c < 56)
          SS_FAIL("short segment");
        {
          uint32_t sections = ss_img_u32(f + o + 48), j;
          if (sections > (c - 56) / 68)
            SS_FAIL("truncated segment sections");
          for (j = 0; j < sections; j++)
            if (ss_img_u32(f + o + 56 + j * 68 + 52))
              SS_FAIL("section relocations unsupported");
        }
        a = ss_img_u32(f + o + 24);
        m = ss_img_u32(f + o + 28);
        b = ss_img_u32(f + o + 36);
        p = ss_img_u32(f + o + 44);
        if (!memcmp(f + o + 8, "__TEXT", 7))
          text = a;
        if (!memcmp(f + o + 8, "__PAGEZERO", 11) ||
            !memcmp(f + o + 8, "__LINKEDIT", 11))
          m = b = 0;
        if (!ss_img_segment(i, a, m, ss_img_u32(f + o + 32), b, p & 7, z))
          SS_FAIL("invalid Mach-O segment");
      } else if (t == 0x80000022 || t == 0x22) {
        unsigned j;
        if (c < 48 || dyld_seen++)
          SS_FAIL("invalid dyld info");
        i->reloc_offset = ss_img_u32(f + o + 8);
        i->reloc_bytes = ss_img_u32(f + o + 12);
        for (j = 16; j < 40; j += 8)
          if (ss_img_u32(f + o + j + 4))
            SS_FAIL("external bindings unsupported");
      } else if (t == 0xb) {
        if (c < 80 || ss_img_u32(f + o + 68) || ss_img_u32(f + o + 76))
          SS_FAIL("legacy dynamic relocations unsupported");
      } else if (t == 0x80000034) {
        SS_FAIL("chained fixups unsupported");
      } else if (t == 0x80000028) {
        if (c < 24 || main_seen++ || ss_img_u32(f + o + 12))
          SS_FAIL("invalid entry command");
        entryoff = ss_img_u32(f + o + 8);
      }
      o += c;
    }
    if (o != doff || !main_seen || !dyld_seen ||
        (uint64_t)text + entryoff > UINT32_MAX ||
        !ss_img_range(z, i->reloc_offset, i->reloc_bytes))
      SS_FAIL("invalid Mach-O metadata");
    i->entry = text + entryoff;
  }
  if (i->low >= i->high || i->high > 0xffff0000u)
    SS_FAIL("invalid image extent");
  {
    int ok = 0;
    for (k = 0; k < i->count; k++) {
      struct ss_image_segment *s = i->segments + k;
      if ((s->protection & 4) && i->entry >= s->address &&
          i->entry - s->address < s->memory)
        ok = 1;
    }
    if (!ok)
      SS_FAIL("entry outside executable segment");
  }
  i->low &= ~65535u;
  i->high = (i->high + 65535u) & ~65535u;
  return 1;
#undef SS_FAIL
}
static int ss_img_uleb(const uint8_t *f, size_t end, size_t *p, uint32_t *v) {
  unsigned shift = 0;
  uint32_t x = 0;
  while (*p < end && shift < 35) {
    uint32_t b = f[(*p)++];
    if (shift == 28 && (b & 0x7f) > 15)
      return 0;
    x |= (b & 127) << shift;
    if (!(b & 128)) {
      *v = x;
      return 1;
    }
    shift += 7;
  }
  return 0;
}
static void ss_img_put(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
/* mapped addresses image.low; only linker-declared native pointer slots move.
   ELF RELA uses explicit addends; Mach-O pointer32 uses in-place addends. */
static int ss_image_relocate(const uint8_t *f, size_t z,
                             const struct ss_image *i, uint8_t *m,
                             uint32_t slide, char *e, size_t en) {
  size_t p = i->reloc_offset, end = p + i->reloc_bytes;
  uint32_t a = 0, type = 0, seg = UINT32_MAX;
  unsigned k;
#define SS_FAIL(s) return ss_img_error(e, en, s)
  if (!ss_img_range(z, i->reloc_offset, i->reloc_bytes) ||
      (uint64_t)i->high + slide > UINT32_MAX)
    SS_FAIL("invalid relocated extent");
  if (!i->format) {
    for (; p < end; p += 12) {
      uint32_t at = ss_img_u32(f + p), info = ss_img_u32(f + p + 4),
               v = ss_img_u32(f + p + 8);
      if (!info)
        continue;
      if (info != 8 || !ss_img_target(i, at, 4) ||
          (uint64_t)v + slide > UINT32_MAX)
        SS_FAIL("unsupported or invalid RELA relocation");
      ss_img_put(m + at - i->low, v + slide);
    }
    return 1;
  }
  while (p < end) {
    uint32_t b = f[p++], op = b & 240, imm = b & 15, count = 0, skip = 0, v = 0;
    if (op == 0) {
      for (; p < end; p++)
        if (f[p])
          SS_FAIL("data after rebase done");
      return 1;
    }
    if (op == 0x10) {
      if (imm != 1)
        SS_FAIL("unsupported rebase type");
      type = imm;
      continue;
    }
    if (op == 0x20) {
      if (imm >= i->count || !ss_img_uleb(f, end, &p, &v) ||
          v > i->segments[imm].memory)
        SS_FAIL("invalid rebase segment");
      seg = imm;
      a = i->segments[seg].address + v;
      continue;
    }
    if (op == 0x30) {
      if (!ss_img_uleb(f, end, &p, &v))
        SS_FAIL("invalid ULEB");
    } else if (op == 0x40)
      v = imm * 4;
    else if (op == 0x50)
      count = imm;
    else if (op == 0x60) {
      if (!ss_img_uleb(f, end, &p, &count))
        SS_FAIL("invalid ULEB");
    } else if (op == 0x70) {
      count = 1;
      if (!ss_img_uleb(f, end, &p, &skip))
        SS_FAIL("invalid ULEB");
    } else if (op == 0x80) {
      if (!ss_img_uleb(f, end, &p, &count) || !ss_img_uleb(f, end, &p, &skip))
        SS_FAIL("invalid ULEB");
    } else
      SS_FAIL("unsupported rebase opcode");
    if (seg == UINT32_MAX)
      SS_FAIL("rebase has no segment");
    if (v) {
      if ((uint64_t)a + v > UINT32_MAX)
        SS_FAIL("rebase overflow");
      a += v;
    }
    if (count > (i->high - i->low) / 4)
      SS_FAIL("excessive rebase count");
    for (k = 0; k < count; k++) {
      const struct ss_image_segment *s = i->segments + seg;
      uint32_t old;
      if (type != 1 || a < s->address || s->memory < 4 ||
          a - s->address > s->memory - 4)
        SS_FAIL("rebase target outside segment");
      old = ss_img_u32(m + a - i->low);
      if ((uint64_t)old + slide > UINT32_MAX)
        SS_FAIL("pointer overflow");
      ss_img_put(m + a - i->low, old + slide);
      if ((uint64_t)a + 4 + skip > UINT32_MAX)
        SS_FAIL("rebase overflow");
      a += 4 + skip;
    }
  }
  SS_FAIL("unterminated rebase stream");
#undef SS_FAIL
}
#endif
