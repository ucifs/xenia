/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SHADER_H_
#define XENIA_GPU_SHADER_H_

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// The structures here are used for both translation and disassembly.
//
// Because disassembly uses them too, to make sure "assemble -> disassemble ->
// reassemble" round trip is always successful with the XNA assembler (as it is
// the accuracy benchmark for translation), only generalization - not
// optimization like nop skipping/replacement - must be done while converting
// microcode to these structures (in other words, parsed shader code should be
// enough to accurately reconstruct the microcode for any shader that could be
// written by a human in assembly).
//
// During the "parsed -> host" part of the translation, however, translators are
// free to make any optimizations (as long as they don't affect the result, of
// course) they find appropriate.

enum class InstructionStorageTarget {
  // Result is not stored.
  kNone,
  // Result is stored to a temporary register indexed by storage_index [0-31].
  kRegister,
  // Result is stored into a vertex shader interpolator export [0-15].
  kInterpolator,
  // Result is stored to the position export (gl_Position).
  kPosition,
  // Result is stored to the vertex shader misc export register, see
  // ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex for description of
  // components.
  kPointSizeEdgeFlagKillVertex,
  // Result is stored as memexport destination address
  // (see xenos::xe_gpu_memexport_stream_t).
  kExportAddress,
  // Result is stored to memexport destination data.
  kExportData,
  // Result is stored to a color target export indexed by storage_index [0-3].
  kColor,
  // X of the result is stored to the depth export (gl_FragDepth).
  kDepth,
};

// Must be used only in translation to skip unused components, but not in
// disassembly (because oPts.x000 will be assembled, but oPts.x00_ has both
// skipped components and zeros, which cannot be encoded, and therefore it will
// not).
constexpr uint32_t GetInstructionStorageTargetUsedComponents(
    InstructionStorageTarget target) {
  switch (target) {
    case InstructionStorageTarget::kNone:
      return 0b0000;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      return 0b0111;
    case InstructionStorageTarget::kDepth:
      return 0b0001;
    default:
      return 0b1111;
  }
}

enum class InstructionStorageAddressingMode {
  // The storage index is not dynamically addressed.
  kStatic,
  // The storage index is addressed by a0.
  kAddressAbsolute,
  // The storage index is addressed by aL.
  kAddressRelative,
};

// Describes the source value of a particular component.
enum class SwizzleSource {
  // Component receives the source X.
  kX,
  // Component receives the source Y.
  kY,
  // Component receives the source Z.
  kZ,
  // Component receives the source W.
  kW,
  // Component receives constant 0.
  k0,
  // Component receives constant 1.
  k1,
};

constexpr SwizzleSource GetSwizzleFromComponentIndex(uint32_t i) {
  return static_cast<SwizzleSource>(i);
}
inline char GetCharForComponentIndex(uint32_t i) {
  const static char kChars[] = {'x', 'y', 'z', 'w'};
  return kChars[i];
}
inline char GetCharForSwizzle(SwizzleSource swizzle_source) {
  const static char kChars[] = {'x', 'y', 'z', 'w', '0', '1'};
  return kChars[static_cast<uint32_t>(swizzle_source)];
}

struct InstructionResult {
  // Where the result is going.
  InstructionStorageTarget storage_target = InstructionStorageTarget::kNone;
  // Index into the storage_target, if it is indexed.
  uint32_t storage_index = 0;
  // How the storage index is dynamically addressed, if it is.
  InstructionStorageAddressingMode storage_addressing_mode =
      InstructionStorageAddressingMode::kStatic;
  // True to clamp the result value to [0-1].
  bool is_clamped = false;
  // Defines whether each output component is written, though this is from the
  // original microcode, not taking into account whether such components
  // actually exist in the target.
  uint32_t original_write_mask = 0b0000;
  // Defines the source for each output component xyzw.
  SwizzleSource components[4] = {SwizzleSource::kX, SwizzleSource::kY,
                                 SwizzleSource::kZ, SwizzleSource::kW};
  // Returns the write mask containing only components actually present in the
  // target.
  uint32_t GetUsedWriteMask() const {
    return original_write_mask &
           GetInstructionStorageTargetUsedComponents(storage_target);
  }
  // True if the components are in their 'standard' swizzle arrangement (xyzw).
  bool IsStandardSwizzle() const {
    return (GetUsedWriteMask() == 0b1111) &&
           components[0] == SwizzleSource::kX &&
           components[1] == SwizzleSource::kY &&
           components[2] == SwizzleSource::kZ &&
           components[3] == SwizzleSource::kW;
  }
  // Returns the components of the result, before swizzling, that won't be
  // discarded or replaced with a constant.
  uint32_t GetUsedResultComponents() const {
    uint32_t used_write_mask = GetUsedWriteMask();
    uint32_t used_components = 0b0000;
    for (uint32_t i = 0; i < 4; ++i) {
      if ((used_write_mask & (1 << i)) && components[i] >= SwizzleSource::kX &&
          components[i] <= SwizzleSource::kW) {
        used_components |=
            1 << (uint32_t(components[i]) - uint32_t(SwizzleSource::kX));
      }
    }
    return used_components;
  }
};

enum class InstructionStorageSource {
  // Source is stored in a temporary register indexed by storage_index [0-31].
  kRegister,
  // Source is stored in a float constant indexed by storage_index [0-511].
  kConstantFloat,
  // Source is stored in a vertex fetch constant indexed by storage_index
  // [0-95].
  kVertexFetchConstant,
  // Source is stored in a texture fetch constant indexed by storage_index
  // [0-31].
  kTextureFetchConstant,
};

struct InstructionOperand {
  // Where the source comes from.
  InstructionStorageSource storage_source = InstructionStorageSource::kRegister;
  // Index into the storage_target, if it is indexed.
  uint32_t storage_index = 0;
  // How the storage index is dynamically addressed, if it is.
  InstructionStorageAddressingMode storage_addressing_mode =
      InstructionStorageAddressingMode::kStatic;
  // True to negate the operand value.
  bool is_negated = false;
  // True to take the absolute value of the source (before any negation).
  bool is_absolute_value = false;
  // Number of components taken from the source operand.
  uint32_t component_count = 4;
  // Defines the source for each component xyzw (up to the given
  // component_count).
  SwizzleSource components[4] = {SwizzleSource::kX, SwizzleSource::kY,
                                 SwizzleSource::kZ, SwizzleSource::kW};
  // Returns the swizzle source for the component, replicating the rightmost
  // component if there are less than 4 components (similar to what the Xbox 360
  // shader compiler does as a general rule for unspecified components).
  SwizzleSource GetComponent(uint32_t index) const {
    return components[std::min(index, component_count - 1)];
  }
  // True if the components are in their 'standard' swizzle arrangement (xyzw).
  bool IsStandardSwizzle() const {
    switch (component_count) {
      case 4:
        return components[0] == SwizzleSource::kX &&
               components[1] == SwizzleSource::kY &&
               components[2] == SwizzleSource::kZ &&
               components[3] == SwizzleSource::kW;
    }
    return false;
  }

  // Returns which components of two operands are identical, but may have
  // different signs (for simplicity of usage with GetComponent, treating the
  // rightmost component as replicated).
  uint32_t GetAbsoluteIdenticalComponents(
      const InstructionOperand& other) const {
    if (storage_source != other.storage_source ||
        storage_index != other.storage_index ||
        storage_addressing_mode != other.storage_addressing_mode) {
      return 0;
    }
    uint32_t identical_components = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      identical_components |= uint32_t(GetComponent(i) == other.GetComponent(i))
                              << i;
    }
    return identical_components;
  }
  // Returns which components of two operands will always be bitwise equal, but
  // may have different signs (disregarding component_count for simplicity of
  // usage with GetComponent, treating the rightmost component as replicated).
  uint32_t GetIdenticalComponents(const InstructionOperand& other) const {
    if (is_negated != other.is_negated ||
        is_absolute_value != other.is_absolute_value) {
      return 0;
    }
    return GetAbsoluteIdenticalComponents(other);
  }
};

struct ParsedExecInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Opcode for the instruction.
  ucode::ControlFlowOpcode opcode;
  // Friendly name of the instruction.
  const char* opcode_name = nullptr;

  // Instruction address where ALU/fetch instructions reside.
  uint32_t instruction_address = 0;
  // Number of instructions to execute.
  uint32_t instruction_count = 0;

  enum class Type {
    // Block is always executed.
    kUnconditional,
    // Execution is conditional on the value of the boolean constant.
    kConditional,
    // Execution is predicated.
    kPredicated,
  };
  // Condition required to execute the instructions.
  Type type = Type::kUnconditional;
  // Constant index used as the conditional if kConditional.
  uint32_t bool_constant_index = 0;
  // Required condition value of the comparision (true or false).
  bool condition = false;

  // Whether this exec ends the shader.
  bool is_end = false;
  // Whether to reset the current predicate.
  bool clean = true;
  // ?
  bool is_yield = false;

  // Sequence bits, 2 per instruction, indicating whether ALU or fetch.
  uint32_t sequence = 0;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedLoopStartInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Integer constant register that holds the loop parameters.
  // Byte-wise: [loop count, start, step [-128, 127], ?]
  uint32_t loop_constant_index = 0;
  // Whether to reuse the current aL instead of reset it to loop start.
  bool is_repeat = false;

  // Target address to jump to when skipping the loop.
  uint32_t loop_skip_address = 0;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedLoopEndInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Break from the loop if the predicate matches the expected value.
  bool is_predicated_break = false;
  // Required condition value of the comparision (true or false).
  bool predicate_condition = false;

  // Integer constant register that holds the loop parameters.
  // Byte-wise: [loop count, start, step [-128, 127], ?]
  uint32_t loop_constant_index = 0;

  // Target address of the start of the loop body.
  uint32_t loop_body_address = 0;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedCallInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Target address.
  uint32_t target_address = 0;

  enum class Type {
    // Call is always made.
    kUnconditional,
    // Call is conditional on the value of the boolean constant.
    kConditional,
    // Call is predicated.
    kPredicated,
  };
  // Condition required to make the call.
  Type type = Type::kUnconditional;
  // Constant index used as the conditional if kConditional.
  uint32_t bool_constant_index = 0;
  // Required condition value of the comparision (true or false).
  bool condition = false;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedReturnInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedJumpInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // Target address.
  uint32_t target_address = 0;

  enum class Type {
    // Jump is always taken.
    kUnconditional,
    // Jump is conditional on the value of the boolean constant.
    kConditional,
    // Jump is predicated.
    kPredicated,
  };
  // Condition required to make the jump.
  Type type = Type::kUnconditional;
  // Constant index used as the conditional if kConditional.
  uint32_t bool_constant_index = 0;
  // Required condition value of the comparision (true or false).
  bool condition = false;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedAllocInstruction {
  // Index into the ucode dword source.
  uint32_t dword_index = 0;

  // The type of resource being allocated.
  ucode::AllocType type = ucode::AllocType::kNone;
  // Total count associated with the allocation.
  int count = 0;

  // True if this allocation is in a vertex shader.
  bool is_vertex_shader = false;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedVertexFetchInstruction {
  // Opcode for the instruction.
  ucode::FetchOpcode opcode;
  // Friendly name of the instruction.
  const char* opcode_name = nullptr;

  // True if the fetch is reusing a previous full fetch.
  // The previous fetch source and constant data will be populated.
  bool is_mini_fetch = false;

  // True if the instruction is predicated on the specified
  // predicate_condition.
  bool is_predicated = false;
  // Expected predication condition value if predicated.
  bool predicate_condition = false;

  // Describes how the instruction result is stored.
  InstructionResult result;

  // Number of source operands.
  size_t operand_count = 0;
  // Describes each source operand.
  InstructionOperand operands[2];

  struct Attributes {
    VertexFormat data_format = VertexFormat::kUndefined;
    int offset = 0;
    int stride = 0;  // In dwords.
    int exp_adjust = 0;
    bool is_index_rounded = false;
    bool is_signed = false;
    bool is_integer = false;
    int prefetch_count = 0;
  };
  // Attributes describing the fetch operation.
  Attributes attributes;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedTextureFetchInstruction {
  // Opcode for the instruction.
  ucode::FetchOpcode opcode;
  // Friendly name of the instruction.
  const char* opcode_name = nullptr;
  // Texture dimension for opcodes that have multiple dimension forms.
  TextureDimension dimension = TextureDimension::k1D;

  // True if the instruction is predicated on the specified
  // predicate_condition.
  bool is_predicated = false;
  // Expected predication condition value if predicated.
  bool predicate_condition = false;

  // True if the instruction has a result.
  bool has_result() const {
    return result.storage_target != InstructionStorageTarget::kNone;
  }
  // Describes how the instruction result is stored.
  InstructionResult result;

  // Number of source operands.
  size_t operand_count = 0;
  // Describes each source operand.
  InstructionOperand operands[2];

  struct Attributes {
    bool fetch_valid_only = true;
    bool unnormalized_coordinates = false;
    TextureFilter mag_filter = TextureFilter::kUseFetchConst;
    TextureFilter min_filter = TextureFilter::kUseFetchConst;
    TextureFilter mip_filter = TextureFilter::kUseFetchConst;
    AnisoFilter aniso_filter = AnisoFilter::kUseFetchConst;
    TextureFilter vol_mag_filter = TextureFilter::kUseFetchConst;
    TextureFilter vol_min_filter = TextureFilter::kUseFetchConst;
    bool use_computed_lod = true;
    bool use_register_lod = false;
    bool use_register_gradients = false;
    float lod_bias = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
  };
  // Attributes describing the fetch operation.
  Attributes attributes;

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

struct ParsedAluInstruction {
  // Opcode for the vector part of the instruction.
  ucode::AluVectorOpcode vector_opcode = ucode::AluVectorOpcode::kAdd;
  // Opcode for the scalar part of the instruction.
  ucode::AluScalarOpcode scalar_opcode = ucode::AluScalarOpcode::kAdds;
  // Friendly name of the vector instruction.
  const char* vector_opcode_name = nullptr;
  // Friendly name of the scalar instruction.
  const char* scalar_opcode_name = nullptr;

  // True if the instruction is predicated on the specified
  // predicate_condition.
  bool is_predicated = false;
  // Expected predication condition value if predicated.
  bool predicate_condition = false;

  // Describes how the vector operation result and, for exports, constant 0/1
  // are stored. For simplicity of translation and disassembly, treating
  // constant 0/1 writes as a part of the vector operation - they need to be
  // expressed somehow in the disassembly anyway with a properly disassembled
  // instruction even if only constants are being exported. The XNA disassembler
  // falls back to displaying the whole vector operation, even if only constant
  // components are written, if the scalar operation is a nop or if the vector
  // operation has side effects (but if the scalar operation isn't nop, it
  // outputs the entire constant mask in the scalar operation destination).
  // Normally the XNA disassembler outputs the constant mask in both vector and
  // scalar operations, but that's not required by assembler, so it doesn't
  // really matter whether it's specified in the vector operation, in the scalar
  // operation, or in both.
  InstructionResult vector_and_constant_result;
  // Describes how the scalar operation result is stored.
  InstructionResult scalar_result;
  // Both operations must be executed before any result is stored if vector and
  // scalar operations are paired. There are cases of vector result being used
  // as scalar operand or vice versa (the halo on Avalanche in Halo 3, for
  // example), in this case there must be no dependency between the two
  // operations.

  // Number of source operands of the vector operation.
  uint32_t vector_operand_count = 0;
  // Describes each source operand of the vector operation.
  InstructionOperand vector_operands[3];
  // Number of source operands of the scalar operation.
  uint32_t scalar_operand_count = 0;
  // Describes each source operand of the scalar operation.
  InstructionOperand scalar_operands[2];

  // Whether the vector part of the instruction is the same as if it was omitted
  // in the assembly (if compiled or assembled with the Xbox 360 shader
  // compiler), and thus reassembling the shader with this instruction omitted
  // will result in the same microcode (since instructions with just an empty
  // write mask may have different values in other fields).
  // This is for disassembly! Translators should use the write masks and
  // AluVectorOpHasSideEffects to skip operations, as this only covers one very
  // specific nop format!
  bool IsVectorOpDefaultNop() const {
    if (vector_opcode != ucode::AluVectorOpcode::kMax ||
        vector_and_constant_result.original_write_mask ||
        vector_and_constant_result.is_clamped ||
        vector_operands[0].storage_source !=
            InstructionStorageSource::kRegister ||
        vector_operands[0].storage_index != 0 ||
        vector_operands[0].storage_addressing_mode !=
            InstructionStorageAddressingMode::kStatic ||
        vector_operands[0].is_negated || vector_operands[0].is_absolute_value ||
        !vector_operands[0].IsStandardSwizzle() ||
        vector_operands[1].storage_source !=
            InstructionStorageSource::kRegister ||
        vector_operands[1].storage_index != 0 ||
        vector_operands[1].storage_addressing_mode !=
            InstructionStorageAddressingMode::kStatic ||
        vector_operands[1].is_negated || vector_operands[1].is_absolute_value ||
        !vector_operands[1].IsStandardSwizzle()) {
      return false;
    }
    if (vector_and_constant_result.storage_target ==
        InstructionStorageTarget::kRegister) {
      if (vector_and_constant_result.storage_index != 0 ||
          vector_and_constant_result.storage_addressing_mode !=
              InstructionStorageAddressingMode::kStatic) {
        return false;
      }
    } else {
      // In case both vector and scalar operations are nop, still need to write
      // somewhere that it's an export, not mov r0._, r0 + retain_prev r0._.
      // Accurate round trip is possible only if the target is o0 or oC0,
      // because if the total write mask is empty, the XNA assembler forces the
      // destination to be o0/oC0, but this doesn't really matter in this case.
      if (IsScalarOpDefaultNop()) {
        return false;
      }
    }
    return true;
  }

  // Whether the scalar part of the instruction is the same as if it was omitted
  // in the assembly (if compiled or assembled with the Xbox 360 shader
  // compiler), and thus reassembling the shader with this instruction omitted
  // will result in the same microcode (since instructions with just an empty
  // write mask may have different values in other fields).
  bool IsScalarOpDefaultNop() const {
    if (scalar_opcode != ucode::AluScalarOpcode::kRetainPrev ||
        scalar_result.original_write_mask || scalar_result.is_clamped) {
      return false;
    }
    if (scalar_result.storage_target == InstructionStorageTarget::kRegister) {
      if (scalar_result.storage_index != 0 ||
          scalar_result.storage_addressing_mode !=
              InstructionStorageAddressingMode::kStatic) {
        return false;
      }
    }
    // For exports, if both are nop, the vector operation will be kept to state
    // in the microcode that the destination in the microcode is an export.
    return true;
  }

  // For translation (not disassembly) - whether this instruction has totally no
  // effect.
  bool IsNop() const {
    return scalar_opcode == ucode::AluScalarOpcode::kRetainPrev &&
           !scalar_result.GetUsedWriteMask() &&
           !vector_and_constant_result.GetUsedWriteMask() &&
           !ucode::AluVectorOpHasSideEffects(vector_opcode);
  }

  // If this is a "normal" eA write recognized by Xenia (MAD with a stream
  // constant), returns the index of the stream float constant, otherwise
  // returns UINT32_MAX.
  uint32_t GetMemExportStreamConstant() const {
    if (vector_and_constant_result.storage_target ==
            InstructionStorageTarget::kExportAddress &&
        vector_opcode == ucode::AluVectorOpcode::kMad &&
        vector_and_constant_result.GetUsedResultComponents() == 0b1111 &&
        !vector_and_constant_result.is_clamped &&
        vector_operands[2].storage_source ==
            InstructionStorageSource::kConstantFloat &&
        vector_operands[2].storage_addressing_mode ==
            InstructionStorageAddressingMode::kStatic &&
        vector_operands[2].IsStandardSwizzle() &&
        !vector_operands[2].is_negated &&
        !vector_operands[2].is_absolute_value) {
      return vector_operands[2].storage_index;
    }
    return UINT32_MAX;
  }

  // Disassembles the instruction into ucode assembly text.
  void Disassemble(StringBuffer* out) const;
};

class Shader {
 public:
  // If values are changed, INVALIDATE SHADER STORAGES (increase their version
  // constexpr) where those are stored! And check bit count where this is
  // packed. This is : uint32_t for simplicity of packing in bit fields.
  enum class HostVertexShaderType : uint32_t {
    kVertex,
    kLineDomainConstant,
    kLineDomainAdaptive,
    kTriangleDomainConstant,
    kTriangleDomainAdaptive,
    kQuadDomainConstant,
    kQuadDomainAdaptive,
  };

  struct Error {
    bool is_fatal = false;
    std::string message;
  };

  struct VertexBinding {
    struct Attribute {
      // Attribute index, 0-based in the entire shader.
      int attrib_index;
      // Fetch instruction with all parameters.
      ParsedVertexFetchInstruction fetch_instr;
      // Size of the attribute, in words.
      uint32_t size_words;
    };

    // Index within the vertex binding listing.
    int binding_index;
    // Fetch constant index [0-95].
    uint32_t fetch_constant;
    // Stride of the entire binding, in words.
    uint32_t stride_words;
    // Packed attributes within the binding buffer.
    std::vector<Attribute> attributes;
  };

  struct TextureBinding {
    // Index within the texture binding listing.
    size_t binding_index;
    // Fetch constant index [0-31].
    uint32_t fetch_constant;
    // Fetch instruction with all parameters.
    ParsedTextureFetchInstruction fetch_instr;
  };

  struct ConstantRegisterMap {
    // Bitmap of all kConstantFloat registers read by the shader.
    // Any shader can only read up to 256 of the 512, and the base is dependent
    // on the shader type and SQ_VS/PS_CONST registers. Each bit corresponds to
    // a storage index from the type base.
    uint64_t float_bitmap[256 / 64];
    // Bitmap of all loop constants read by the shader.
    // Each bit corresponds to a storage index [0-31].
    uint32_t loop_bitmap;
    // Bitmap of all bool constants read by the shader.
    // Each bit corresponds to a storage index [0-255].
    uint32_t bool_bitmap[256 / 32];

    // Total number of kConstantFloat registers read by the shader.
    uint32_t float_count;

    // Whether kConstantFloat registers are indexed dynamically - in this case,
    // float_bitmap must be set to all 1, and tight packing must not be done.
    bool float_dynamic_addressing;

    // Returns the index of the float4 constant as if all float4 constant
    // registers actually referenced were tightly packed in a buffer, or
    // UINT32_MAX if not found.
    uint32_t GetPackedFloatConstantIndex(uint32_t float_constant) const {
      if (float_constant >= 256) {
        return UINT32_MAX;
      }
      if (float_dynamic_addressing) {
        // Any can potentially be read - not packing.
        return float_constant;
      }
      uint32_t block_index = float_constant / 64;
      uint32_t bit_index = float_constant % 64;
      if (!(float_bitmap[block_index] & (uint64_t(1) << bit_index))) {
        return UINT32_MAX;
      }
      uint32_t offset = 0;
      for (uint32_t i = 0; i < block_index; ++i) {
        offset += xe::bit_count(float_bitmap[i]);
      }
      return offset + xe::bit_count(float_bitmap[block_index] &
                                    ((uint64_t(1) << bit_index) - 1));
    }
  };

  Shader(ShaderType shader_type, uint64_t ucode_data_hash,
         const uint32_t* ucode_dwords, size_t ucode_dword_count);
  virtual ~Shader();

  // Whether the shader is identified as a vertex or pixel shader.
  ShaderType type() const { return shader_type_; }

  // If this is a vertex shader, and it has been translated, type of the shader
  // in a D3D11-like rendering pipeline - shader interface depends on in, so it
  // must be known at translation time.
  HostVertexShaderType host_vertex_shader_type() const {
    return host_vertex_shader_type_;
  }

  // Microcode dwords in host endianness.
  const std::vector<uint32_t>& ucode_data() const { return ucode_data_; }
  uint64_t ucode_data_hash() const { return ucode_data_hash_; }
  const uint32_t* ucode_dwords() const { return ucode_data_.data(); }
  size_t ucode_dword_count() const { return ucode_data_.size(); }

  // All vertex bindings used in the shader.
  // Valid for vertex shaders only.
  const std::vector<VertexBinding>& vertex_bindings() const {
    return vertex_bindings_;
  }

  // All texture bindings used in the shader.
  // Valid for both vertex and pixel shaders.
  const std::vector<TextureBinding>& texture_bindings() const {
    return texture_bindings_;
  }

  // Bitmaps of all constant registers accessed by the shader.
  const ConstantRegisterMap& constant_register_map() const {
    return constant_register_map_;
  }

  // All c# registers used as the addend in MAD operations to eA.
  const std::vector<uint32_t>& memexport_stream_constants() const {
    return memexport_stream_constants_;
  }

  // Returns true if the given color target index [0-3].
  bool writes_color_target(uint32_t i) const {
    return writes_color_targets_[i];
  }

  // True if the shader overrides the pixel depth.
  bool writes_depth() const { return writes_depth_; }

  // True if Xenia can automatically enable early depth/stencil for the pixel
  // shader when RB_DEPTHCONTROL EARLY_Z_ENABLE is not set, provided alpha
  // testing and alpha to coverage are disabled.
  bool implicit_early_z_allowed() const { return implicit_early_z_allowed_; }

  // True if the shader was translated and prepared without error.
  bool is_valid() const { return is_valid_; }

  // True if the shader has already been translated.
  bool is_translated() const { return is_translated_; }

  // Errors that occurred during translation.
  const std::vector<Error>& errors() const { return errors_; }

  // Microcode disassembly in D3D format.
  const std::string& ucode_disassembly() const { return ucode_disassembly_; }

  // Translated shader binary (or text).
  const std::vector<uint8_t>& translated_binary() const {
    return translated_binary_;
  }

  // Gets the translated shader binary as a string.
  // This is only valid if it is actually text.
  std::string GetTranslatedBinaryString() const;

  // Disassembly of the translated from the host graphics layer.
  // May be empty if the host does not support disassembly.
  const std::string& host_disassembly() const { return host_disassembly_; }
  // A lot of errors that occurred during preparation of the host shader.
  const std::string& host_error_log() const { return host_error_log_; }
  // Host binary that can be saved and reused across runs.
  // May be empty if the host does not support saving binaries.
  const std::vector<uint8_t>& host_binary() const { return host_binary_; }

  // Dumps the shader to a file in the given path based on ucode hash.
  // Both the ucode binary and disassembled and translated shader will be
  // written.
  // Returns the filename of the shader and the binary.
  std::pair<std::filesystem::path, std::filesystem::path> Dump(
      const std::filesystem::path& base_path, const char* path_prefix);

 protected:
  friend class ShaderTranslator;

  ShaderType shader_type_;
  HostVertexShaderType host_vertex_shader_type_ = HostVertexShaderType::kVertex;
  std::vector<uint32_t> ucode_data_;
  uint64_t ucode_data_hash_;

  std::vector<VertexBinding> vertex_bindings_;
  std::vector<TextureBinding> texture_bindings_;
  ConstantRegisterMap constant_register_map_ = {0};
  bool writes_color_targets_[4] = {false, false, false, false};
  bool writes_depth_ = false;
  bool implicit_early_z_allowed_ = true;
  std::vector<uint32_t> memexport_stream_constants_;

  bool is_valid_ = false;
  bool is_translated_ = false;
  std::vector<Error> errors_;

  std::string ucode_disassembly_;
  std::vector<uint8_t> translated_binary_;
  std::string host_disassembly_;
  std::string host_error_log_;
  std::vector<uint8_t> host_binary_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SHADER_H_
