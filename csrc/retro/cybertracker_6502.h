// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-retro: a 6502 processor, as far as music routines need one: all documented
// instructions with decimal mode, the undocumented ones that players and crunchers
// use, and a count of the cycles spent. No interrupts: whoever owns the machine
// calls the routines himself (see call()).
//
// The owner of the memory is a class with
//   uint8_t read(uint16_t address);  void write(uint16_t address, uint8_t value);
#pragma once

#include <cstdint>

namespace retro {

template <class Bus>
class Cpu6502 {
 public:
  explicit Cpu6502(Bus& bus) : bus_(bus) {}

  uint8_t a = 0, x = 0, y = 0, sp = 0xff;
  uint16_t pc = 0;
  bool n = false, v = false, d = false, i = true, z = false, c = false;
  uint64_t cycles = 0;
  bool jammed = false;        // met an instruction that stops the processor

  uint8_t flags() const {
    return (n ? 0x80 : 0) | (v ? 0x40 : 0) | 0x20 | (d ? 8 : 0) | (i ? 4 : 0) | (z ? 2 : 0) | (c ? 1 : 0);
  }
  void set_flags(uint8_t p) {
    n = p & 0x80; v = p & 0x40; d = p & 8; i = p & 4; z = p & 2; c = p & 1;
  }

  void push(uint8_t value) { bus_.write(0x100 | sp--, value); }
  uint8_t pull() { return bus_.read(0x100 | ++sp); }

  // Runs the subroutine at <address> until it returns, or until <limit> instructions
  // have passed. True when it returned.
  bool call(uint16_t address, uint64_t limit) {
    const uint8_t start = sp;
    // the return address is one that no program has, so that RTS is seen here
    push(0xff);
    push(0xf1);
    pc = address;
    for (uint64_t k = 0; k < limit && !jammed; k++) {
      step();
      if (pc == 0xfff2 && sp == start) return true;
    }
    return false;
  }

  void step() {
    const uint8_t op = fetch();
    cycles += kCycles[op];
    switch (op) {
      // load and store
      case 0xa9: lda(imm()); break;   case 0xa5: lda(zp()); break;    case 0xb5: lda(zpx()); break;
      case 0xad: lda(abs_()); break;  case 0xbd: lda(absx(true)); break;  case 0xb9: lda(absy(true)); break;
      case 0xa1: lda(indx()); break;  case 0xb1: lda(indy(true)); break;
      case 0xa2: ldx(imm()); break;   case 0xa6: ldx(zp()); break;    case 0xb6: ldx(zpy()); break;
      case 0xae: ldx(abs_()); break;  case 0xbe: ldx(absy(true)); break;
      case 0xa0: ldy(imm()); break;   case 0xa4: ldy(zp()); break;    case 0xb4: ldy(zpx()); break;
      case 0xac: ldy(abs_()); break;  case 0xbc: ldy(absx(true)); break;
      case 0x85: bus_.write(zp(), a); break;     case 0x95: bus_.write(zpx(), a); break;
      case 0x8d: bus_.write(abs_(), a); break;   case 0x9d: bus_.write(absx(false), a); break;
      case 0x99: bus_.write(absy(false), a); break;
      case 0x81: bus_.write(indx(), a); break;   case 0x91: bus_.write(indy(false), a); break;
      case 0x86: bus_.write(zp(), x); break;     case 0x96: bus_.write(zpy(), x); break;
      case 0x8e: bus_.write(abs_(), x); break;
      case 0x84: bus_.write(zp(), y); break;     case 0x94: bus_.write(zpx(), y); break;
      case 0x8c: bus_.write(abs_(), y); break;
      // transfers
      case 0xaa: x = nz(a); break;    case 0xa8: y = nz(a); break;    case 0x8a: a = nz(x); break;
      case 0x98: a = nz(y); break;    case 0xba: x = nz(sp); break;   case 0x9a: sp = x; break;
      // stack
      case 0x48: push(a); break;      case 0x68: a = nz(pull()); break;
      case 0x08: push(flags() | 0x10); break;    case 0x28: set_flags(pull()); break;
      // logic
      case 0x29: and_(imm()); break;  case 0x25: and_(zp()); break;   case 0x35: and_(zpx()); break;
      case 0x2d: and_(abs_()); break; case 0x3d: and_(absx(true)); break; case 0x39: and_(absy(true)); break;
      case 0x21: and_(indx()); break; case 0x31: and_(indy(true)); break;
      case 0x09: ora(imm()); break;   case 0x05: ora(zp()); break;    case 0x15: ora(zpx()); break;
      case 0x0d: ora(abs_()); break;  case 0x1d: ora(absx(true)); break;  case 0x19: ora(absy(true)); break;
      case 0x01: ora(indx()); break;  case 0x11: ora(indy(true)); break;
      case 0x49: eor(imm()); break;   case 0x45: eor(zp()); break;    case 0x55: eor(zpx()); break;
      case 0x4d: eor(abs_()); break;  case 0x5d: eor(absx(true)); break;  case 0x59: eor(absy(true)); break;
      case 0x41: eor(indx()); break;  case 0x51: eor(indy(true)); break;
      case 0x24: bit(zp()); break;    case 0x2c: bit(abs_()); break;
      // arithmetic
      case 0x69: adc(bus_.read(imm())); break;   case 0x65: adc(bus_.read(zp())); break;
      case 0x75: adc(bus_.read(zpx())); break;   case 0x6d: adc(bus_.read(abs_())); break;
      case 0x7d: adc(bus_.read(absx(true))); break;  case 0x79: adc(bus_.read(absy(true))); break;
      case 0x61: adc(bus_.read(indx())); break;  case 0x71: adc(bus_.read(indy(true))); break;
      case 0xe9: case 0xeb: sbc(bus_.read(imm())); break;
      case 0xe5: sbc(bus_.read(zp())); break;
      case 0xf5: sbc(bus_.read(zpx())); break;   case 0xed: sbc(bus_.read(abs_())); break;
      case 0xfd: sbc(bus_.read(absx(true))); break;  case 0xf9: sbc(bus_.read(absy(true))); break;
      case 0xe1: sbc(bus_.read(indx())); break;  case 0xf1: sbc(bus_.read(indy(true))); break;
      case 0xc9: cmp(a, imm()); break;   case 0xc5: cmp(a, zp()); break;   case 0xd5: cmp(a, zpx()); break;
      case 0xcd: cmp(a, abs_()); break;  case 0xdd: cmp(a, absx(true)); break; case 0xd9: cmp(a, absy(true)); break;
      case 0xc1: cmp(a, indx()); break;  case 0xd1: cmp(a, indy(true)); break;
      case 0xe0: cmp(x, imm()); break;   case 0xe4: cmp(x, zp()); break;   case 0xec: cmp(x, abs_()); break;
      case 0xc0: cmp(y, imm()); break;   case 0xc4: cmp(y, zp()); break;   case 0xcc: cmp(y, abs_()); break;
      // increments
      case 0xe6: inc(zp()); break;    case 0xf6: inc(zpx()); break;   case 0xee: inc(abs_()); break;
      case 0xfe: inc(absx(false)); break;
      case 0xc6: dec(zp()); break;    case 0xd6: dec(zpx()); break;   case 0xce: dec(abs_()); break;
      case 0xde: dec(absx(false)); break;
      case 0xe8: x = nz(x + 1); break;   case 0xc8: y = nz(y + 1); break;
      case 0xca: x = nz(x - 1); break;   case 0x88: y = nz(y - 1); break;
      // shifts
      case 0x0a: a = asl(a); break;   case 0x06: rmw(zp(), &Cpu6502::asl); break;
      case 0x16: rmw(zpx(), &Cpu6502::asl); break;   case 0x0e: rmw(abs_(), &Cpu6502::asl); break;
      case 0x1e: rmw(absx(false), &Cpu6502::asl); break;
      case 0x4a: a = lsr(a); break;   case 0x46: rmw(zp(), &Cpu6502::lsr); break;
      case 0x56: rmw(zpx(), &Cpu6502::lsr); break;   case 0x4e: rmw(abs_(), &Cpu6502::lsr); break;
      case 0x5e: rmw(absx(false), &Cpu6502::lsr); break;
      case 0x2a: a = rol(a); break;   case 0x26: rmw(zp(), &Cpu6502::rol); break;
      case 0x36: rmw(zpx(), &Cpu6502::rol); break;   case 0x2e: rmw(abs_(), &Cpu6502::rol); break;
      case 0x3e: rmw(absx(false), &Cpu6502::rol); break;
      case 0x6a: a = ror(a); break;   case 0x66: rmw(zp(), &Cpu6502::ror); break;
      case 0x76: rmw(zpx(), &Cpu6502::ror); break;   case 0x6e: rmw(abs_(), &Cpu6502::ror); break;
      case 0x7e: rmw(absx(false), &Cpu6502::ror); break;
      // jumps
      case 0x4c: pc = abs_(); break;
      case 0x6c: {
        const uint16_t at = abs_();
        // the famous fault: the high byte comes from the same page
        pc = bus_.read(at) | bus_.read((at & 0xff00) | ((at + 1) & 0xff)) << 8;
        break;
      }
      case 0x20: {
        const uint16_t to = abs_();
        const uint16_t back = pc - 1;
        push(back >> 8);
        push(back & 0xff);
        pc = to;
        break;
      }
      case 0x60: { const uint8_t lo = pull(); pc = (lo | pull() << 8) + 1; break; }
      case 0x40: { set_flags(pull()); const uint8_t lo = pull(); pc = lo | pull() << 8; break; }
      case 0x00: {
        const uint16_t back = pc + 1;
        push(back >> 8);
        push(back & 0xff);
        push(flags() | 0x10);
        i = true;
        pc = bus_.read(0xfffe) | bus_.read(0xffff) << 8;
        break;
      }
      case 0x10: branch(!n); break;   case 0x30: branch(n); break;
      case 0x50: branch(!v); break;   case 0x70: branch(v); break;
      case 0x90: branch(!c); break;   case 0xb0: branch(c); break;
      case 0xd0: branch(!z); break;   case 0xf0: branch(z); break;
      // flags
      case 0x18: c = false; break;    case 0x38: c = true; break;
      case 0x58: i = false; break;    case 0x78: i = true; break;
      case 0xd8: d = false; break;    case 0xf8: d = true; break;
      case 0xb8: v = false; break;
      case 0xea: break;

      // undocumented, the stable ones
      case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa: break;          // NOP
      case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: imm(); break;              // NOP #
      case 0x04: case 0x44: case 0x64: zp(); break;                                     // NOP zp
      case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: zpx(); break;   // NOP zp,x
      case 0x0c: abs_(); break;                                                         // NOP abs
      case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: absx(true); break;
      case 0xa7: lax(zp()); break;    case 0xb7: lax(zpy()); break;   case 0xaf: lax(abs_()); break;
      case 0xbf: lax(absy(true)); break; case 0xa3: lax(indx()); break; case 0xb3: lax(indy(true)); break;
      case 0x87: bus_.write(zp(), a & x); break;    case 0x97: bus_.write(zpy(), a & x); break;
      case 0x8f: bus_.write(abs_(), a & x); break;  case 0x83: bus_.write(indx(), a & x); break;
      case 0xc7: dcp(zp()); break;    case 0xd7: dcp(zpx()); break;   case 0xcf: dcp(abs_()); break;
      case 0xdf: dcp(absx(false)); break; case 0xdb: dcp(absy(false)); break;
      case 0xc3: dcp(indx()); break;  case 0xd3: dcp(indy(false)); break;
      case 0xe7: isb(zp()); break;    case 0xf7: isb(zpx()); break;   case 0xef: isb(abs_()); break;
      case 0xff: isb(absx(false)); break; case 0xfb: isb(absy(false)); break;
      case 0xe3: isb(indx()); break;  case 0xf3: isb(indy(false)); break;
      case 0x07: slo(zp()); break;    case 0x17: slo(zpx()); break;   case 0x0f: slo(abs_()); break;
      case 0x1f: slo(absx(false)); break; case 0x1b: slo(absy(false)); break;
      case 0x03: slo(indx()); break;  case 0x13: slo(indy(false)); break;
      case 0x27: rla(zp()); break;    case 0x37: rla(zpx()); break;   case 0x2f: rla(abs_()); break;
      case 0x3f: rla(absx(false)); break; case 0x3b: rla(absy(false)); break;
      case 0x23: rla(indx()); break;  case 0x33: rla(indy(false)); break;
      case 0x47: sre(zp()); break;    case 0x57: sre(zpx()); break;   case 0x4f: sre(abs_()); break;
      case 0x5f: sre(absx(false)); break; case 0x5b: sre(absy(false)); break;
      case 0x43: sre(indx()); break;  case 0x53: sre(indy(false)); break;
      case 0x67: rra(zp()); break;    case 0x77: rra(zpx()); break;   case 0x6f: rra(abs_()); break;
      case 0x7f: rra(absx(false)); break; case 0x7b: rra(absy(false)); break;
      case 0x63: rra(indx()); break;  case 0x73: rra(indy(false)); break;
      case 0x0b: case 0x2b: and_(imm()); c = n; break;                                  // ANC
      case 0x4b: and_(imm()); a = lsr(a); break;                                        // ALR
      case 0x6b: {                                                                      // ARR
        and_(imm());
        a = nz((a >> 1) | (c ? 0x80 : 0));
        c = a & 0x40;
        v = ((a >> 6) ^ (a >> 5)) & 1;
        break;
      }
      case 0xcb: {                                                                      // SBX
        const unsigned value = bus_.read(imm());
        const unsigned both = a & x;
        c = both >= value;
        x = nz(both - value);
        break;
      }
      default:
        jammed = true;
        pc--;
        break;
    }
  }

 private:
  typedef uint8_t (Cpu6502::*Shift)(uint8_t);

  uint8_t fetch() { return bus_.read(pc++); }
  uint16_t imm() { return pc++; }
  uint16_t zp() { return fetch(); }
  uint16_t zpx() { return (fetch() + x) & 0xff; }
  uint16_t zpy() { return (fetch() + y) & 0xff; }
  uint16_t abs_() { const uint8_t lo = fetch(); return lo | fetch() << 8; }
  uint16_t absx(bool extra) { return indexed(abs_(), x, extra); }
  uint16_t absy(bool extra) { return indexed(abs_(), y, extra); }
  uint16_t indx() {
    const uint8_t at = fetch() + x;
    return bus_.read(at) | bus_.read((at + 1) & 0xff) << 8;
  }
  uint16_t indy(bool extra) {
    const uint8_t at = fetch();
    return indexed(bus_.read(at) | bus_.read((at + 1) & 0xff) << 8, y, extra);
  }
  uint16_t indexed(uint16_t base, uint8_t index, bool extra) {
    const uint16_t to = base + index;
    if (extra && ((to ^ base) & 0xff00)) cycles++;   // a page was crossed
    return to;
  }

  uint8_t nz(unsigned value) {
    value &= 0xff;
    n = value & 0x80;
    z = value == 0;
    return value;
  }
  void lda(uint16_t at) { a = nz(bus_.read(at)); }
  void ldx(uint16_t at) { x = nz(bus_.read(at)); }
  void ldy(uint16_t at) { y = nz(bus_.read(at)); }
  void lax(uint16_t at) { a = x = nz(bus_.read(at)); }
  void and_(uint16_t at) { a = nz(a & bus_.read(at)); }
  void ora(uint16_t at) { a = nz(a | bus_.read(at)); }
  void eor(uint16_t at) { a = nz(a ^ bus_.read(at)); }
  void bit(uint16_t at) {
    const uint8_t value = bus_.read(at);
    n = value & 0x80;
    v = value & 0x40;
    z = (a & value) == 0;
  }
  void cmp(uint8_t reg, uint16_t at) {
    const unsigned value = bus_.read(at);
    c = reg >= value;
    nz(reg - value);
  }
  // Instructions that change memory write the old value back first, as the real
  // processor does: "INC $D019" acknowledges an interrupt that way.
  uint8_t old(uint16_t at) {
    const uint8_t value = bus_.read(at);
    bus_.write(at, value);
    return value;
  }
  void inc(uint16_t at) { bus_.write(at, nz(old(at) + 1)); }
  void dec(uint16_t at) { bus_.write(at, nz(old(at) - 1)); }
  uint8_t asl(uint8_t value) { c = value & 0x80; return nz(value << 1); }
  uint8_t lsr(uint8_t value) { c = value & 1; return nz(value >> 1); }
  uint8_t rol(uint8_t value) {
    const unsigned out = value << 1 | (c ? 1 : 0);
    c = value & 0x80;
    return nz(out);
  }
  uint8_t ror(uint8_t value) {
    const unsigned out = value >> 1 | (c ? 0x80 : 0);
    c = value & 1;
    return nz(out);
  }
  void rmw(uint16_t at, Shift how) { bus_.write(at, (this->*how)(old(at))); }
  void dcp(uint16_t at) {
    const uint8_t value = bus_.read(at) - 1;
    bus_.write(at, value);
    c = a >= value;
    nz(a - value);
  }
  void isb(uint16_t at) {
    const uint8_t value = bus_.read(at) + 1;
    bus_.write(at, value);
    sbc(value);
  }
  void slo(uint16_t at) {
    const uint8_t value = asl(bus_.read(at));
    bus_.write(at, value);
    a = nz(a | value);
  }
  void rla(uint16_t at) {
    const uint8_t value = rol(bus_.read(at));
    bus_.write(at, value);
    a = nz(a & value);
  }
  void sre(uint16_t at) {
    const uint8_t value = lsr(bus_.read(at));
    bus_.write(at, value);
    a = nz(a ^ value);
  }
  void rra(uint16_t at) {
    const uint8_t value = ror(bus_.read(at));
    bus_.write(at, value);
    adc(value);
  }
  void branch(bool taken) {
    const int8_t by = static_cast<int8_t>(fetch());
    if (!taken) return;
    const uint16_t to = pc + by;
    cycles += ((to ^ pc) & 0xff00) ? 2 : 1;
    pc = to;
  }
  void adc(uint8_t value) {
    const unsigned carry = c ? 1 : 0;
    if (!d) {
      const unsigned sum = a + value + carry;
      v = (~(a ^ value) & (a ^ sum) & 0x80) != 0;
      c = sum > 0xff;
      a = nz(sum);
      return;
    }
    unsigned lo = (a & 0x0f) + (value & 0x0f) + carry;
    unsigned hi = (a & 0xf0) + (value & 0xf0);
    z = ((a + value + carry) & 0xff) == 0;
    if (lo > 9) { lo += 6; hi += 0x10; }
    n = hi & 0x80;
    v = (~(a ^ value) & (a ^ hi) & 0x80) != 0;
    if (hi > 0x90) hi += 0x60;
    c = hi > 0xff;
    a = (lo & 0x0f) | (hi & 0xf0);
  }
  void sbc(uint8_t value) {
    const unsigned borrow = c ? 0 : 1;
    const unsigned diff = a - value - borrow;
    const bool overflow = ((a ^ value) & (a ^ diff) & 0x80) != 0;
    if (d) {
      int lo = (a & 0x0f) - (value & 0x0f) - static_cast<int>(borrow);
      int hi = (a >> 4) - (value >> 4);
      if (lo < 0) { lo += 10; hi--; }
      if (hi < 0) hi += 10;
      nz(diff);
      a = static_cast<uint8_t>(static_cast<unsigned>(hi) << 4 | (lo & 0x0f));  // hi may be below 0 with digits above 9
    } else {
      a = nz(diff);
    }
    v = overflow;
    c = diff < 0x100;
  }

  Bus& bus_;

  static constexpr uint8_t kCycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
  };
};

}  // namespace retro
