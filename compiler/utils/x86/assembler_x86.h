/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_UTILS_X86_ASSEMBLER_X86_H_
#define ART_COMPILER_UTILS_X86_ASSEMBLER_X86_H_

#include <vector>

#include "arch/x86/instruction_set_features_x86.h"
#include "base/arena_containers.h"
#include "base/array_ref.h"
#include "base/bit_utils.h"
#include "base/enums.h"
#include "base/globals.h"
#include "base/macros.h"
#include "constants_x86.h"
#include "heap_poisoning.h"
#include "managed_register_x86.h"
#include "offsets.h"
#include "utils/assembler.h"

namespace art HIDDEN {
namespace x86 {

class Immediate : public ValueObject {
 public:
  explicit Immediate(int32_t value_in) : value_(value_in) {}

  int32_t value() const { return value_; }

  bool is_int8() const { return IsInt<8>(value_); }
  bool is_uint8() const { return IsUint<8>(value_); }
  bool is_int16() const { return IsInt<16>(value_); }
  bool is_uint16() const { return IsUint<16>(value_); }

 private:
  const int32_t value_;
};


class Operand : public ValueObject {
 public:
  uint8_t mod() const {
    return (encoding_at(0) >> 6) & 3;
  }

  Register rm() const {
    return static_cast<Register>(encoding_at(0) & 7);
  }

  ScaleFactor scale() const {
    return static_cast<ScaleFactor>((encoding_at(1) >> 6) & 3);
  }

  Register index() const {
    return static_cast<Register>((encoding_at(1) >> 3) & 7);
  }

  Register base() const {
    return static_cast<Register>(encoding_at(1) & 7);
  }

  int32_t disp() const {
    return disp_;
  }

  int8_t disp8() const {
    CHECK_GE(length_, 2);
    return static_cast<int8_t>(encoding_[length_ - 1]);
  }

  int32_t disp32() const {
    CHECK_GE(length_, 5);
    int32_t value;
    memcpy(&value, &encoding_[length_ - 4], sizeof(value));
    return value;
  }

  bool IsRegister(Register reg) const {
    return ((encoding_[0] & 0xF8) == 0xC0)  // Addressing mode is register only.
        && ((encoding_[0] & 0x07) == reg);  // Register codes match.
  }

  inline bool operator==(const Operand &op) const {
    return length_ == op.length_ &&
        memcmp(encoding_, op.encoding_, length_) == 0 &&
        disp_ == op.disp_ &&
        fixup_ == op.fixup_;
  }

 protected:
  // Operand can be sub classed (e.g: Address).
  Operand() : length_(0), disp_(0), fixup_(nullptr) { }

  void SetModRM(int mod_in, Register rm_in) {
    CHECK_EQ(mod_in & ~3, 0);
    encoding_[0] = (mod_in << 6) | rm_in;
    length_ = 1;
  }

  void SetSIB(ScaleFactor scale_in, Register index_in, Register base_in) {
    CHECK_EQ(length_, 1);
    CHECK_EQ(scale_in & ~3, 0);
    encoding_[1] = (scale_in << 6) | (index_in << 3) | base_in;
    length_ = 2;
  }

  void SetDisp8(int8_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    encoding_[length_++] = static_cast<uint8_t>(disp);
    disp_ = disp;
  }

  void SetDisp32(int32_t disp) {
    CHECK(length_ == 1 || length_ == 2);
    int disp_size = sizeof(disp);
    memmove(&encoding_[length_], &disp, disp_size);
    length_ += disp_size;
    disp_ = disp;
  }

  AssemblerFixup* GetFixup() const {
    return fixup_;
  }

  void SetFixup(AssemblerFixup* fixup) {
    fixup_ = fixup;
  }

 private:
  uint8_t length_;
  uint8_t encoding_[6];
  int32_t disp_;

  // A fixup can be associated with the operand, in order to be applied after the
  // code has been generated. This is used for constant area fixups.
  AssemblerFixup* fixup_;

  explicit Operand(Register reg) : disp_(0), fixup_(nullptr) { SetModRM(3, reg); }

  // Get the operand encoding byte at the given index.
  uint8_t encoding_at(int index_in) const {
    CHECK_GE(index_in, 0);
    CHECK_LT(index_in, length_);
    return encoding_[index_in];
  }

  friend class X86Assembler;
};


class Address : public Operand {
 public:
  Address(Register base_in, int32_t disp) {
    Init(base_in, disp);
  }

  Address(Register base_in, int32_t disp, AssemblerFixup *fixup) {
    Init(base_in, disp);
    SetFixup(fixup);
  }

  Address(Register base_in, Offset disp) {
    Init(base_in, disp.Int32Value());
  }

  Address(Register base_in, FrameOffset disp) {
    CHECK_EQ(base_in, ESP);
    Init(ESP, disp.Int32Value());
  }

  Address(Register base_in, MemberOffset disp) {
    Init(base_in, disp.Int32Value());
  }

  Address(Register index_in, ScaleFactor scale_in, int32_t disp) {
    CHECK_NE(index_in, ESP);  // Illegal addressing mode.
    SetModRM(0, ESP);
    SetSIB(scale_in, index_in, EBP);
    SetDisp32(disp);
  }

  Address(Register base_in, Register index_in, ScaleFactor scale_in, int32_t disp) {
    Init(base_in, index_in, scale_in, disp);
  }

  Address(Register base_in,
          Register index_in,
          ScaleFactor scale_in,
          int32_t disp, AssemblerFixup *fixup) {
    Init(base_in, index_in, scale_in, disp);
    SetFixup(fixup);
  }

  // Break the address into pieces and reassemble it again with a new displacement.
  // Note that it may require a new addressing mode if displacement size is changed.
  static Address displace(const Address &addr, int32_t disp) {
    const int32_t new_disp = addr.disp() + disp;
    const bool sib = addr.rm() == ESP;
    const bool ebp = EBP == (sib ? addr.base() : addr.rm());
    Address new_addr;
    if (addr.mod() == 0 && ebp) {
      // Special case: mod 00b and EBP in r/m or SIB base => 32-bit displacement.
      new_addr.SetModRM(0, addr.rm());
      if (sib) {
        new_addr.SetSIB(addr.scale(), addr.index(), addr.base());
      }
      new_addr.SetDisp32(new_disp);
    } else if (new_disp == 0 && !ebp) {
      // Mod 00b (excluding a special case for EBP) => no displacement.
      new_addr.SetModRM(0, addr.rm());
      if (sib) {
        new_addr.SetSIB(addr.scale(), addr.index(), addr.base());
      }
    } else if (new_disp >= -128 && new_disp <= 127) {
      // Mod 01b => 8-bit displacement.
      new_addr.SetModRM(1, addr.rm());
      if (sib) {
        new_addr.SetSIB(addr.scale(), addr.index(), addr.base());
      }
      new_addr.SetDisp8(new_disp);
    } else {
      // Mod 10b => 32-bit displacement.
      new_addr.SetModRM(2, addr.rm());
      if (sib) {
        new_addr.SetSIB(addr.scale(), addr.index(), addr.base());
      }
      new_addr.SetDisp32(new_disp);
    }
    new_addr.SetFixup(addr.GetFixup());
    return new_addr;
  }

  Register GetBaseRegister() {
    if (rm() == ESP) {
      return base();
    } else {
      return rm();
    }
  }

  static Address Absolute(uintptr_t addr) {
    Address result;
    result.SetModRM(0, EBP);
    result.SetDisp32(addr);
    return result;
  }

  static Address Absolute(ThreadOffset32 addr) {
    return Absolute(addr.Int32Value());
  }

  inline bool operator==(const Address& addr) const {
    return static_cast<const Operand&>(*this) == static_cast<const Operand&>(addr);
  }

 private:
  Address() {}

  void Init(Register base_in, int32_t disp) {
    if (disp == 0 && base_in != EBP) {
      SetModRM(0, base_in);
      if (base_in == ESP) SetSIB(TIMES_1, ESP, base_in);
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, base_in);
      if (base_in == ESP) SetSIB(TIMES_1, ESP, base_in);
      SetDisp8(disp);
    } else {
      SetModRM(2, base_in);
      if (base_in == ESP) SetSIB(TIMES_1, ESP, base_in);
      SetDisp32(disp);
    }
  }

  void Init(Register base_in, Register index_in, ScaleFactor scale_in, int32_t disp) {
    CHECK_NE(index_in, ESP);  // Illegal addressing mode.
    if (disp == 0 && base_in != EBP) {
      SetModRM(0, ESP);
      SetSIB(scale_in, index_in, base_in);
    } else if (disp >= -128 && disp <= 127) {
      SetModRM(1, ESP);
      SetSIB(scale_in, index_in, base_in);
      SetDisp8(disp);
    } else {
      SetModRM(2, ESP);
      SetSIB(scale_in, index_in, base_in);
      SetDisp32(disp);
    }
  }
};

std::ostream& operator<<(std::ostream& os, const Address& addr);

// This is equivalent to the Label class, used in a slightly different context. We
// inherit the functionality of the Label class, but prevent unintended
// derived-to-base conversions by making the base class private.
class NearLabel : private Label {
 public:
  NearLabel() : Label() {}

  // Expose the Label routines that we need.
  using Label::Position;
  using Label::LinkPosition;
  using Label::IsBound;
  using Label::IsUnused;
  using Label::IsLinked;

 private:
  using Label::BindTo;
  using Label::LinkTo;

  friend class x86::X86Assembler;

  DISALLOW_COPY_AND_ASSIGN(NearLabel);
};

/**
 * Class to handle constant area values.
 */
class ConstantArea {
 public:
  explicit ConstantArea(ArenaAllocator* allocator)
      : buffer_(allocator->Adapter(kArenaAllocAssembler)) {}

  // Add a double to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddDouble(double v);

  // Add a float to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddFloat(float v);

  // Add an int32_t to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddInt32(int32_t v);

  // Add an int32_t to the end of the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AppendInt32(int32_t v);

  // Add an int64_t to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddInt64(int64_t v);

  bool IsEmpty() const {
    return buffer_.size() == 0;
  }

  size_t GetSize() const {
    return buffer_.size() * elem_size_;
  }

  ArrayRef<const int32_t> GetBuffer() const {
    return ArrayRef<const int32_t>(buffer_);
  }

 private:
  static constexpr size_t elem_size_ = sizeof(int32_t);
  ArenaVector<int32_t> buffer_;
};

class X86Assembler final : public Assembler {
 public:
  explicit X86Assembler(ArenaAllocator* allocator,
                        const X86InstructionSetFeatures* instruction_set_features = nullptr)
                : Assembler(allocator),
                  constant_area_(allocator),
                  has_AVX_(instruction_set_features != nullptr ? instruction_set_features->HasAVX() : false),
                  has_AVX2_(instruction_set_features != nullptr ? instruction_set_features->HasAVX2() :false) {}
  virtual ~X86Assembler() {}

  /*
   * Emit Machine Instructions.
   */
  void call(Register reg);
  void call(const Address& address);
  void call(Label* label);
  void call(const ExternalLabel& label);

  void pushl(Register reg);
  void pushl(const Address& address);
  void pushl(const Immediate& imm);

  void popl(Register reg);
  void popl(const Address& address);

  void movl(Register dst, const Immediate& src);
  void movl(Register dst, Register src);

  void movl(Register dst, const Address& src);
  void movl(const Address& dst, Register src);
  void movl(const Address& dst, const Immediate& imm);
  void movl(const Address& dst, Label* lbl);

  void movntl(const Address& dst, Register src);

  void blsi(Register dst, Register src);  // no addr variant (for now)
  void blsmsk(Register dst, Register src);  // no addr variant (for now)
  void blsr(Register dst, Register src);  // no addr varianr (for now)

  void bswapl(Register dst);

  void bsfl(Register dst, Register src);
  void bsfl(Register dst, const Address& src);
  void bsrl(Register dst, Register src);
  void bsrl(Register dst, const Address& src);

  void popcntl(Register dst, Register src);
  void popcntl(Register dst, const Address& src);

  void rorl(Register reg, const Immediate& imm);
  void rorl(Register operand, Register shifter);
  void roll(Register reg, const Immediate& imm);
  void roll(Register operand, Register shifter);

  void movzxb(Register dst, ByteRegister src);
  void movzxb(Register dst, const Address& src);
  void movsxb(Register dst, ByteRegister src);
  void movsxb(Register dst, const Address& src);
  void movb(Register dst, const Address& src);
  void movb(const Address& dst, ByteRegister src);
  void movb(const Address& dst, const Immediate& imm);

  void movzxw(Register dst, Register src);
  void movzxw(Register dst, const Address& src);
  void movsxw(Register dst, Register src);
  void movsxw(Register dst, const Address& src);
  void movw(Register dst, const Address& src);
  void movw(const Address& dst, Register src);
  void movw(const Address& dst, const Immediate& imm);

  void leal(Register dst, const Address& src);

  void cmovl(Condition condition, Register dst, Register src);
  void cmovl(Condition condition, Register dst, const Address& src);

  void setb(Condition condition, Register dst);

  void movaps(XmmRegister dst, XmmRegister src);     // move
  void movaps(XmmRegister dst, const Address& src);  // load aligned
  void movups(XmmRegister dst, const Address& src);  // load unaligned
  void movaps(const Address& dst, XmmRegister src);  // store aligned
  void movups(const Address& dst, XmmRegister src);  // store unaligned

  void vmovaps(XmmRegister dst, XmmRegister src);     // move
  void vmovaps(XmmRegister dst, const Address& src);  // load aligned
  void vmovups(XmmRegister dst, const Address& src);  // load unaligned
  void vmovaps(const Address& dst, XmmRegister src);  // store aligned
  void vmovups(const Address& dst, XmmRegister src);  // store unaligned

  void movss(XmmRegister dst, const Address& src);
  void movss(const Address& dst, XmmRegister src);
  void movss(XmmRegister dst, XmmRegister src);

  void movd(XmmRegister dst, Register src);
  void movd(Register dst, XmmRegister src);

  void addss(XmmRegister dst, XmmRegister src);
  void addss(XmmRegister dst, const Address& src);
  void subss(XmmRegister dst, XmmRegister src);
  void subss(XmmRegister dst, const Address& src);
  void mulss(XmmRegister dst, XmmRegister src);
  void mulss(XmmRegister dst, const Address& src);
  void divss(XmmRegister dst, XmmRegister src);
  void divss(XmmRegister dst, const Address& src);

  void addps(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void subps(XmmRegister dst, XmmRegister src);
  void mulps(XmmRegister dst, XmmRegister src);
  void divps(XmmRegister dst, XmmRegister src);

  void vmulps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vmulpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vdivps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vdivpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void vaddps(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);
  void vsubps(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);
  void vsubpd(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);
  void vaddpd(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);

  void vfmadd213ss(XmmRegister acc, XmmRegister left, XmmRegister right);
  void vfmadd213sd(XmmRegister acc, XmmRegister left, XmmRegister right);

  void movapd(XmmRegister dst, XmmRegister src);     // move
  void movapd(XmmRegister dst, const Address& src);  // load aligned
  void movupd(XmmRegister dst, const Address& src);  // load unaligned
  void movapd(const Address& dst, XmmRegister src);  // store aligned
  void movupd(const Address& dst, XmmRegister src);  // store unaligned

  void vmovapd(XmmRegister dst, XmmRegister src);     // move
  void vmovapd(XmmRegister dst, const Address& src);  // load aligned
  void vmovupd(XmmRegister dst, const Address& src);  // load unaligned
  void vmovapd(const Address& dst, XmmRegister src);  // store aligned
  void vmovupd(const Address& dst, XmmRegister src);  // store unaligned

  void movsd(XmmRegister dst, const Address& src);
  void movsd(const Address& dst, XmmRegister src);
  void movsd(XmmRegister dst, XmmRegister src);

  void movhpd(XmmRegister dst, const Address& src);
  void movhpd(const Address& dst, XmmRegister src);

  void addsd(XmmRegister dst, XmmRegister src);
  void addsd(XmmRegister dst, const Address& src);
  void subsd(XmmRegister dst, XmmRegister src);
  void subsd(XmmRegister dst, const Address& src);
  void mulsd(XmmRegister dst, XmmRegister src);
  void mulsd(XmmRegister dst, const Address& src);
  void divsd(XmmRegister dst, XmmRegister src);
  void divsd(XmmRegister dst, const Address& src);

  void addpd(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void subpd(XmmRegister dst, XmmRegister src);
  void mulpd(XmmRegister dst, XmmRegister src);
  void divpd(XmmRegister dst, XmmRegister src);

  void movdqa(XmmRegister dst, XmmRegister src);     // move
  void movdqa(XmmRegister dst, const Address& src);  // load aligned
  void movdqu(XmmRegister dst, const Address& src);  // load unaligned
  void movdqa(const Address& dst, XmmRegister src);  // store aligned
  void movdqu(const Address& dst, XmmRegister src);  // store unaligned

  void vmovdqa(XmmRegister dst, XmmRegister src);     // move
  void vmovdqa(XmmRegister dst, const Address& src);  // load aligned
  void vmovdqu(XmmRegister dst, const Address& src);  // load unaligned
  void vmovdqa(const Address& dst, XmmRegister src);  // store aligned
  void vmovdqu(const Address& dst, XmmRegister src);  // store unaligned

  void paddb(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void psubb(XmmRegister dst, XmmRegister src);

  void vpaddb(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);
  void vpaddw(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);

  void paddw(XmmRegister dst, XmmRegister src);
  void psubw(XmmRegister dst, XmmRegister src);
  void pmullw(XmmRegister dst, XmmRegister src);
  void vpmullw(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void vpsubb(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vpsubw(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vpsubd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void paddd(XmmRegister dst, XmmRegister src);
  void psubd(XmmRegister dst, XmmRegister src);
  void pmulld(XmmRegister dst, XmmRegister src);

  void vpmulld(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void vpaddd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void paddq(XmmRegister dst, XmmRegister src);
  void psubq(XmmRegister dst, XmmRegister src);

  void vpaddq(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);
  void vpsubq(XmmRegister dst, XmmRegister add_left, XmmRegister add_right);

  void paddusb(XmmRegister dst, XmmRegister src);
  void paddsb(XmmRegister dst, XmmRegister src);
  void paddusw(XmmRegister dst, XmmRegister src);
  void paddsw(XmmRegister dst, XmmRegister src);
  void psubusb(XmmRegister dst, XmmRegister src);
  void psubsb(XmmRegister dst, XmmRegister src);
  void psubusw(XmmRegister dst, XmmRegister src);
  void psubsw(XmmRegister dst, XmmRegister src);

  void cvtsi2ss(XmmRegister dst, Register src);
  void cvtsi2sd(XmmRegister dst, Register src);

  void cvtss2si(Register dst, XmmRegister src);
  void cvtss2sd(XmmRegister dst, XmmRegister src);

  void cvtsd2si(Register dst, XmmRegister src);
  void cvtsd2ss(XmmRegister dst, XmmRegister src);

  void cvttss2si(Register dst, XmmRegister src);
  void cvttsd2si(Register dst, XmmRegister src);

  void cvtdq2ps(XmmRegister dst, XmmRegister src);
  void cvtdq2pd(XmmRegister dst, XmmRegister src);

  void comiss(XmmRegister a, XmmRegister b);
  void comiss(XmmRegister a, const Address& b);
  void comisd(XmmRegister a, XmmRegister b);
  void comisd(XmmRegister a, const Address& b);
  void ucomiss(XmmRegister a, XmmRegister b);
  void ucomiss(XmmRegister a, const Address& b);
  void ucomisd(XmmRegister a, XmmRegister b);
  void ucomisd(XmmRegister a, const Address& b);

  void roundsd(XmmRegister dst, XmmRegister src, const Immediate& imm);
  void roundss(XmmRegister dst, XmmRegister src, const Immediate& imm);

  void sqrtsd(XmmRegister dst, XmmRegister src);
  void sqrtss(XmmRegister dst, XmmRegister src);

  void xorpd(XmmRegister dst, const Address& src);
  void xorpd(XmmRegister dst, XmmRegister src);
  void xorps(XmmRegister dst, const Address& src);
  void xorps(XmmRegister dst, XmmRegister src);
  void pxor(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void vpxor(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vxorps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vxorpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void andpd(XmmRegister dst, XmmRegister src);
  void andpd(XmmRegister dst, const Address& src);
  void andps(XmmRegister dst, XmmRegister src);
  void andps(XmmRegister dst, const Address& src);
  void pand(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void vpand(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vandps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vandpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void andn(Register dst, Register src1, Register src2);  // no addr variant (for now)
  void andnpd(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void andnps(XmmRegister dst, XmmRegister src);
  void pandn(XmmRegister dst, XmmRegister src);
  void vpandn(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vandnps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vandnpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void orpd(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void orps(XmmRegister dst, XmmRegister src);
  void por(XmmRegister dst, XmmRegister src);
  void vpor(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vorps(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void vorpd(XmmRegister dst, XmmRegister src1, XmmRegister src2);

  void pavgb(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void pavgw(XmmRegister dst, XmmRegister src);
  void psadbw(XmmRegister dst, XmmRegister src);
  void pmaddwd(XmmRegister dst, XmmRegister src);
  void vpmaddwd(XmmRegister dst, XmmRegister src1, XmmRegister src2);
  void phaddw(XmmRegister dst, XmmRegister src);
  void phaddd(XmmRegister dst, XmmRegister src);
  void haddps(XmmRegister dst, XmmRegister src);
  void haddpd(XmmRegister dst, XmmRegister src);
  void phsubw(XmmRegister dst, XmmRegister src);
  void phsubd(XmmRegister dst, XmmRegister src);
  void hsubps(XmmRegister dst, XmmRegister src);
  void hsubpd(XmmRegister dst, XmmRegister src);

  void pminsb(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void pmaxsb(XmmRegister dst, XmmRegister src);
  void pminsw(XmmRegister dst, XmmRegister src);
  void pmaxsw(XmmRegister dst, XmmRegister src);
  void pminsd(XmmRegister dst, XmmRegister src);
  void pmaxsd(XmmRegister dst, XmmRegister src);

  void pminub(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void pmaxub(XmmRegister dst, XmmRegister src);
  void pminuw(XmmRegister dst, XmmRegister src);
  void pmaxuw(XmmRegister dst, XmmRegister src);
  void pminud(XmmRegister dst, XmmRegister src);
  void pmaxud(XmmRegister dst, XmmRegister src);

  void minps(XmmRegister dst, XmmRegister src);  // no addr variant (for now)
  void maxps(XmmRegister dst, XmmRegister src);
  void minpd(XmmRegister dst, XmmRegister src);
  void maxpd(XmmRegister dst, XmmRegister src);

  void pcmpeqb(XmmRegister dst, XmmRegister src);
  void pcmpeqw(XmmRegister dst, XmmRegister src);
  void pcmpeqd(XmmRegister dst, XmmRegister src);
  void pcmpeqq(XmmRegister dst, XmmRegister src);

  void pcmpgtb(XmmRegister dst, XmmRegister src);
  void pcmpgtw(XmmRegister dst, XmmRegister src);
  void pcmpgtd(XmmRegister dst, XmmRegister src);
  void pcmpgtq(XmmRegister dst, XmmRegister src);  // SSE4.2

  void shufpd(XmmRegister dst, XmmRegister src, const Immediate& imm);
  void shufps(XmmRegister dst, XmmRegister src, const Immediate& imm);
  void pshufd(XmmRegister dst, XmmRegister src, const Immediate& imm);

  void punpcklbw(XmmRegister dst, XmmRegister src);
  void punpcklwd(XmmRegister dst, XmmRegister src);
  void punpckldq(XmmRegister dst, XmmRegister src);
  void punpcklqdq(XmmRegister dst, XmmRegister src);

  void punpckhbw(XmmRegister dst, XmmRegister src);
  void punpckhwd(XmmRegister dst, XmmRegister src);
  void punpckhdq(XmmRegister dst, XmmRegister src);
  void punpckhqdq(XmmRegister dst, XmmRegister src);

  void psllw(XmmRegister reg, const Immediate& shift_count);
  void pslld(XmmRegister reg, const Immediate& shift_count);
  void psllq(XmmRegister reg, const Immediate& shift_count);

  void psraw(XmmRegister reg, const Immediate& shift_count);
  void psrad(XmmRegister reg, const Immediate& shift_count);
  // no psraq

  void psrlw(XmmRegister reg, const Immediate& shift_count);
  void psrld(XmmRegister reg, const Immediate& shift_count);
  void psrlq(XmmRegister reg, const Immediate& shift_count);
  void psrldq(XmmRegister reg, const Immediate& shift_count);

  void flds(const Address& src);
  void fstps(const Address& dst);
  void fsts(const Address& dst);

  void fldl(const Address& src);
  void fstpl(const Address& dst);
  void fstl(const Address& dst);

  void fstsw();

  void fucompp();

  void fnstcw(const Address& dst);
  void fldcw(const Address& src);

  void fistpl(const Address& dst);
  void fistps(const Address& dst);
  void fildl(const Address& src);
  void filds(const Address& src);

  void fincstp();
  void ffree(const Immediate& index);

  void fsin();
  void fcos();
  void fptan();
  void fprem();

  void xchgb(ByteRegister dst, ByteRegister src);
  void xchgb(ByteRegister reg, const Address& address);

  // Wrappers for `xchgb` that accept `Register` instead of `ByteRegister` (used for testing).
  void xchgb(Register dst, Register src);
  void xchgb(Register reg, const Address& address);

  void xchgw(Register dst, Register src);
  void xchgw(Register reg, const Address& address);

  void xchgl(Register dst, Register src);
  void xchgl(Register reg, const Address& address);

  void cmpb(const Address& address, const Immediate& imm);
  void cmpw(const Address& address, const Immediate& imm);

  void cmpl(Register reg, const Immediate& imm);
  void cmpl(Register reg0, Register reg1);
  void cmpl(Register reg, const Address& address);

  void cmpl(const Address& address, Register reg);
  void cmpl(const Address& address, const Immediate& imm);

  void testl(Register reg1, Register reg2);
  void testl(Register reg, const Immediate& imm);
  void testl(Register reg1, const Address& address);

  void testb(const Address& dst, const Immediate& imm);
  void testl(const Address& dst, const Immediate& imm);

  void andl(Register dst, const Immediate& imm);
  void andl(Register dst, Register src);
  void andl(Register dst, const Address& address);
  void andw(const Address& address, const Immediate& imm);

  void orl(Register dst, const Immediate& imm);
  void orl(Register dst, Register src);
  void orl(Register dst, const Address& address);

  void xorl(Register dst, Register src);
  void xorl(Register dst, const Immediate& imm);
  void xorl(Register dst, const Address& address);

  void addl(Register dst, Register src);
  void addl(Register reg, const Immediate& imm);
  void addl(Register reg, const Address& address);

  void addl(const Address& address, Register reg);
  void addl(const Address& address, const Immediate& imm);
  void addw(const Address& address, const Immediate& imm);
  void addw(Register reg, const Immediate& imm);

  void adcl(Register dst, Register src);
  void adcl(Register reg, const Immediate& imm);
  void adcl(Register dst, const Address& address);

  void subl(Register dst, Register src);
  void subl(Register reg, const Immediate& imm);
  void subl(Register reg, const Address& address);
  void subl(const Address& address, Register src);

  void cdq();

  void idivl(Register reg);
  void divl(Register reg);

  void imull(Register dst, Register src);
  void imull(Register reg, const Immediate& imm);
  void imull(Register dst, Register src, const Immediate& imm);
  void imull(Register reg, const Address& address);

  void imull(Register reg);
  void imull(const Address& address);

  void mull(Register reg);
  void mull(const Address& address);

  void sbbl(Register dst, Register src);
  void sbbl(Register reg, const Immediate& imm);
  void sbbl(Register reg, const Address& address);
  void sbbl(const Address& address, Register src);

  void incl(Register reg);
  void incl(const Address& address);

  void decl(Register reg);
  void decl(const Address& address);

  void shll(Register reg, const Immediate& imm);
  void shll(Register operand, Register shifter);
  void shll(const Address& address, const Immediate& imm);
  void shll(const Address& address, Register shifter);
  void shrl(Register reg, const Immediate& imm);
  void shrl(Register operand, Register shifter);
  void shrl(const Address& address, const Immediate& imm);
  void shrl(const Address& address, Register shifter);
  void sarl(Register reg, const Immediate& imm);
  void sarl(Register operand, Register shifter);
  void sarl(const Address& address, const Immediate& imm);
  void sarl(const Address& address, Register shifter);
  void shld(Register dst, Register src, Register shifter);
  void shld(Register dst, Register src, const Immediate& imm);
  void shrd(Register dst, Register src, Register shifter);
  void shrd(Register dst, Register src, const Immediate& imm);

  void negl(Register reg);
  void notl(Register reg);

  void enter(const Immediate& imm);
  void leave();

  void ret();
  void ret(const Immediate& imm);

  void nop();
  void int3();
  void hlt();

  void j(Condition condition, Label* label);
  void j(Condition condition, NearLabel* label);
  void jecxz(NearLabel* label);

  void jmp(Register reg);
  void jmp(const Address& address);
  void jmp(Label* label);
  void jmp(NearLabel* label);

  void repne_scasb();
  void repne_scasw();
  void repe_cmpsb();
  void repe_cmpsw();
  void repe_cmpsl();
  void rep_movsb();
  void rep_movsl();
  void rep_movsw();

  X86Assembler* lock();
  void cmpxchgb(const Address& address, ByteRegister reg);
  void cmpxchgw(const Address& address, Register reg);
  void cmpxchgl(const Address& address, Register reg);
  void cmpxchg8b(const Address& address);

  void xaddb(const Address& address, ByteRegister reg);
  void xaddw(const Address& address, Register reg);
  void xaddl(const Address& address, Register reg);

  void mfence();

  X86Assembler* fs();
  X86Assembler* gs();

  //
  // Macros for High-level operations.
  //

  void AddImmediate(Register reg, const Immediate& imm);

  void LoadLongConstant(XmmRegister dst, int64_t value);
  void LoadDoubleConstant(XmmRegister dst, double value);

  // For testing purpose (Repeat* functions expect Register rather than ByteRegister).
  void cmpxchgb(const Address& address, Register reg) {
    cmpxchgb(address, static_cast<ByteRegister>(reg));
  }

  // For testing purpose (Repeat* functions expect Register rather than ByteRegister).
  void LockCmpxchgb(const Address& address, Register reg) {
    LockCmpxchgb(address, static_cast<ByteRegister>(reg));
  }

  void LockCmpxchgb(const Address& address, ByteRegister reg) {
    lock()->cmpxchgb(address, reg);
  }

  void LockCmpxchgw(const Address& address, Register reg) {
    AssemblerBuffer::EnsureCapacity ensured(&buffer_);
    // We make sure that the operand size override bytecode is emited before the lock bytecode.
    // We test against clang which enforces this bytecode order.
    EmitOperandSizeOverride();
    EmitUint8(0xF0);
    EmitUint8(0x0F);
    EmitUint8(0xB1);
    EmitOperand(reg, address);
  }

  void LockCmpxchgl(const Address& address, Register reg) {
    lock()->cmpxchgl(address, reg);
  }

  void LockCmpxchg8b(const Address& address) {
    lock()->cmpxchg8b(address);
  }

  // For testing purpose (Repeat* functions expect Register rather than ByteRegister).
  void LockXaddb(const Address& address, Register reg) {
    LockXaddb(address, static_cast<ByteRegister>(reg));
  }

  void LockXaddb(const Address& address, ByteRegister reg) {
    lock()->xaddb(address, reg);
  }

  void LockXaddw(const Address& address, Register reg) {
    AssemblerBuffer::EnsureCapacity ensured(&buffer_);
    // We make sure that the operand size override bytecode is emited before the lock bytecode.
    // We test against clang which enforces this bytecode order.
    EmitOperandSizeOverride();
    EmitUint8(0xF0);
    EmitUint8(0x0F);
    EmitUint8(0xC1);
    EmitOperand(reg, address);
  }

  void LockXaddl(const Address& address, Register reg) {
    lock()->xaddl(address, reg);
  }

  //
  // Misc. functionality
  //
  int PreferredLoopAlignment() { return 16; }
  void Align(int alignment, int offset);
  void Bind(Label* label) override;
  void Jump(Label* label) override {
    jmp(label);
  }
  void Bind(NearLabel* label);

  //
  // Heap poisoning.
  //

  // Poison a heap reference contained in `reg`.
  void PoisonHeapReference(Register reg) { negl(reg); }
  // Unpoison a heap reference contained in `reg`.
  void UnpoisonHeapReference(Register reg) { negl(reg); }
  // Poison a heap reference contained in `reg` if heap poisoning is enabled.
  void MaybePoisonHeapReference(Register reg) {
    if (kPoisonHeapReferences) {
      PoisonHeapReference(reg);
    }
  }
  // Unpoison a heap reference contained in `reg` if heap poisoning is enabled.
  void MaybeUnpoisonHeapReference(Register reg) {
    if (kPoisonHeapReferences) {
      UnpoisonHeapReference(reg);
    }
  }

  // Add a double to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddDouble(double v) { return constant_area_.AddDouble(v); }

  // Add a float to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddFloat(float v)   { return constant_area_.AddFloat(v); }

  // Add an int32_t to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddInt32(int32_t v) {
    return constant_area_.AddInt32(v);
  }

  // Add an int32_t to the end of the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AppendInt32(int32_t v) {
    return constant_area_.AppendInt32(v);
  }

  // Add an int64_t to the constant area, returning the offset into
  // the constant area where the literal resides.
  size_t AddInt64(int64_t v) { return constant_area_.AddInt64(v); }

  // Add the contents of the constant area to the assembler buffer.
  void AddConstantArea();

  // Is the constant area empty? Return true if there are no literals in the constant area.
  bool IsConstantAreaEmpty() const { return constant_area_.IsEmpty(); }

  // Return the current size of the constant area.
  size_t ConstantAreaSize() const { return constant_area_.GetSize(); }

  bool CpuHasAVXorAVX2FeatureFlag();

 private:
  inline void EmitUint8(uint8_t value);
  inline void EmitInt32(int32_t value);
  inline void EmitRegisterOperand(int rm, int reg);
  inline void EmitXmmRegisterOperand(int rm, XmmRegister reg);
  inline void EmitFixup(AssemblerFixup* fixup);
  inline void EmitOperandSizeOverride();

  void EmitOperand(int rm, const Operand& operand);
  void EmitImmediate(const Immediate& imm, bool is_16_op = false);
  void EmitComplex(
      int rm, const Operand& operand, const Immediate& immediate, bool is_16_op = false);
  void EmitLabel(Label* label, int instruction_size);
  void EmitLabelLink(Label* label);
  void EmitLabelLink(NearLabel* label);

  void EmitGenericShift(int rm, const Operand& operand, const Immediate& imm);
  void EmitGenericShift(int rm, const Operand& operand, Register shifter);

  uint8_t EmitVexPrefixByteZero(bool is_twobyte_form);
  uint8_t EmitVexPrefixByteOne(bool R, bool X, bool B, int SET_VEX_M);
  uint8_t EmitVexPrefixByteOne(bool R,
                               X86ManagedRegister operand,
                               int SET_VEX_L,
                               int SET_VEX_PP);
  uint8_t EmitVexPrefixByteTwo(bool W,
                               X86ManagedRegister operand,
                               int SET_VEX_L,
                               int SET_VEX_PP);
  uint8_t EmitVexPrefixByteTwo(bool W,
                               int SET_VEX_L,
                               int SET_VEX_PP);

  // Helper function to emit a shorter variant of XCHG for when at least one operand is EAX/AX.
  bool try_xchg_eax(Register dst, Register src);

  ConstantArea constant_area_;
  bool has_AVX_;     // x86 256bit SIMD AVX.
  bool has_AVX2_;    // x86 256bit SIMD AVX 2.0.

  DISALLOW_COPY_AND_ASSIGN(X86Assembler);
};

inline void X86Assembler::EmitUint8(uint8_t value) {
  buffer_.Emit<uint8_t>(value);
}

inline void X86Assembler::EmitInt32(int32_t value) {
  buffer_.Emit<int32_t>(value);
}

inline void X86Assembler::EmitRegisterOperand(int rm, int reg) {
  CHECK_GE(rm, 0);
  CHECK_LT(rm, 8);
  buffer_.Emit<uint8_t>(0xC0 + (rm << 3) + reg);
}

inline void X86Assembler::EmitXmmRegisterOperand(int rm, XmmRegister reg) {
  EmitRegisterOperand(rm, static_cast<Register>(reg));
}

inline void X86Assembler::EmitFixup(AssemblerFixup* fixup) {
  buffer_.EmitFixup(fixup);
}

inline void X86Assembler::EmitOperandSizeOverride() {
  EmitUint8(0x66);
}

}  // namespace x86
}  // namespace art

#endif  // ART_COMPILER_UTILS_X86_ASSEMBLER_X86_H_
