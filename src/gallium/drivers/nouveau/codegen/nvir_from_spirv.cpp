/*
 * Copyright 2017 Pierre Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <cstring>
#include <functional>
#include <set>
#include <stack>
#include <unordered_map>
#include <vector>

#include <spirv-tools/libspirv.h>

#include "codegen/nv50_ir.h"
#include "codegen/nv50_ir_util.h"
#include "codegen/nv50_ir_build_util.h"

#include "OpenCL.std.h"
#include "gallium/state_trackers/clover/spirv/spirv.hpp"

namespace spirv {

using word = unsigned int;
using Words = std::vector<word>;

static inline bool hasFlag(spv::ImageOperandsMask v, spv::ImageOperandsShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::FPFastMathModeMask v, spv::FPFastMathModeShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::SelectionControlMask v, spv::SelectionControlShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::LoopControlMask v, spv::LoopControlShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::FunctionControlMask v, spv::FunctionControlShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::MemorySemanticsMask v, spv::MemorySemanticsShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::MemoryAccessMask v, spv::MemoryAccessShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }
static inline bool hasFlag(spv::KernelProfilingInfoMask v, spv::KernelProfilingInfoShift f) { return static_cast<uint32_t>(v) & (1u << static_cast<uint32_t>(f)); }

// TODO(pmoreau): Use parsedOperand’s type to deduce the cast rather than
//                having to specify it.
template<typename T>
T getOperand(const spv_parsed_instruction_t *parsedInstruction, uint16_t operandIndex)
{
   assert(operandIndex < parsedInstruction->num_operands);

   const spv_parsed_operand_t& parsedOperand = parsedInstruction->operands[operandIndex];
   T value;
   assert(sizeof(T) <= parsedOperand.num_words * sizeof(word));
   std::memcpy(&value, parsedInstruction->words + parsedOperand.offset, sizeof(T));

   return value;
}

template<>
const char* getOperand<const char*>(const spv_parsed_instruction_t *parsedInstruction, uint16_t operandIndex)
{
   assert(operandIndex < parsedInstruction->num_operands);

   const spv_parsed_operand_t parsedOperand = parsedInstruction->operands[operandIndex];
   assert(parsedOperand.type == SPV_OPERAND_TYPE_LITERAL_STRING);

   return reinterpret_cast<const char*>(parsedInstruction->words + parsedOperand.offset);
}

int isSrcSigned(spv::Op opcode)
{
   switch (opcode)
   {
   case spv::Op::OpSGreaterThan:
   case spv::Op::OpSGreaterThanEqual:
   case spv::Op::OpSLessThan:
   case spv::Op::OpSLessThanEqual:
   case spv::Op::OpSDiv:
   case spv::Op::OpSMod:
   case spv::Op::OpSRem:
   case spv::Op::OpAtomicSMin:
   case spv::Op::OpAtomicSMax:
   case spv::Op::OpSConvert:
   case spv::Op::OpConvertFToU:
   case spv::Op::OpConvertSToF:
   case spv::Op::OpConvertFToS:
   case spv::Op::OpSatConvertSToU:
      return 1;
   case spv::Op::OpUGreaterThan:
   case spv::Op::OpUGreaterThanEqual:
   case spv::Op::OpULessThan:
   case spv::Op::OpULessThanEqual:
   case spv::Op::OpUDiv:
   case spv::Op::OpUMod:
   case spv::Op::OpAtomicUMin:
   case spv::Op::OpAtomicUMax:
   case spv::Op::OpUConvert:
   case spv::Op::OpConvertUToF:
   case spv::Op::OpConvertPtrToU:
   case spv::Op::OpSatConvertUToS:
   case spv::Op::OpConvertUToPtr:
      return 0;
   default:
      return -1;
   }
}

int isDstSigned(spv::Op opcode)
{
   switch (opcode)
   {
   case spv::Op::OpSConvert:
   case spv::Op::OpConvertUToF:
   case spv::Op::OpConvertSToF:
   case spv::Op::OpConvertFToS:
   case spv::Op::OpSatConvertUToS:
      return 1;
   case spv::Op::OpUConvert:
   case spv::Op::OpConvertFToU:
   case spv::Op::OpConvertPtrToU:
   case spv::Op::OpSatConvertSToU:
   case spv::Op::OpConvertUToPtr:
      return 0;
   default:
      return -1;
   }
}

} // namespace spirv


namespace {

using namespace spirv;
using namespace nv50_ir;

class Converter : public BuildUtil
{
public:
   struct EntryPoint {
      uint32_t index;
      spv::ExecutionModel executionModel;
      std::string name;
      std::vector<spv::Id> interface;
   };
   enum class SpirvFile { NONE, TEMPORARY, SHARED, GLOBAL, CONST, INPUT, PREDICATE, IMMEDIATE };
   using Decoration = std::unordered_map<spv::Decoration, std::vector<Words>>;
   using Decorations = std::unordered_map<spv::Id, Decoration>;
    struct PValue {
      union { Value *value; Value *indirect; };
      Symbol *symbol;
      PValue() : value(nullptr), symbol(nullptr) {}
      PValue(Value *value) : value(value), symbol(nullptr) {}
      PValue(Symbol *symbol, Value *indirect) : indirect(indirect), symbol(symbol) {}
      bool isUndefined() const { return symbol == nullptr && value == nullptr; }
      bool isValue() const { return value != nullptr && (value->reg.file == FILE_GPR || value->reg.file == FILE_IMMEDIATE); }
   };
   class Type {
   public:
      Type(spv::Op type) : type(type), id(0u) {}
      virtual ~Type() {}
      spv::Id getId() const { return id; }
      virtual spv::Op getType() const { return type; }
      virtual bool isBasicType() const { return false; }
      virtual bool isCompooundType() const { return false; }
      virtual bool isVoidType() const { return false; }
      virtual std::vector<ImmediateValue *> generateConstant(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const { assert(false); return std::vector<ImmediateValue *>(); }
      virtual std::vector<ImmediateValue *> generateBoolConstant(Converter &conv, bool value) const { assert(false); return std::vector<ImmediateValue *>(); }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const = 0;
      virtual unsigned int getSize(void) const { assert(false); return 0u; }
      unsigned int getAlignment() const { return alignment; }
      virtual std::vector<unsigned int> getPaddings() const { return { 0u }; }
      virtual enum DataType getEnumType(int isSigned = -1) const { assert(false); return DataType::TYPE_NONE; }
      virtual unsigned int getElementsNb(void) const { return 1u; }
      virtual unsigned int getElementSize(unsigned int /*index*/) const { return getSize(); }
      virtual Type const* getElementType(unsigned int /*index*/) const { return this; }
      virtual enum DataType getElementEnumType(unsigned int /*index*/, int isSigned = -1) const { return getEnumType(isSigned); }
      virtual unsigned int getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position = 0u) const { return 0u; }
      virtual void getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position = 0u) const { if (position < ids.size()) assert(false); }
      virtual bool isVectorOfSize(unsigned int /*size*/) const { return false; }

      const spv::Op type;
      spv::Id id;
      unsigned int alignment;
   };
   struct SpirVValue {
      SpirvFile storageFile;
      Type const* type;
      std::vector<PValue> value;
      std::vector<unsigned int> paddings; // How to align each component: this will be used by OpCopyMemory* for example
      bool isPacked;
      SpirVValue() : storageFile(SpirvFile::NONE), type(nullptr), value(), paddings(), isPacked(false) {}
      SpirVValue(SpirvFile sf, const Type *t, const std::vector<PValue> &v, const std::vector<unsigned int> &p, bool ip = false) : storageFile(sf), type(t), value(v), paddings(p), isPacked(ip) {}
      bool isUndefined() const { return type == nullptr; }
      Value * getValue(BuildUtil *bld, unsigned int i) const {
         const PValue &pvalue = value[i];
         Value *value = pvalue.value;
         if (storageFile == SpirvFile::IMMEDIATE) {
            value = bld->getScratch(pvalue.value->reg.size);
            bld->mkMov(value, pvalue.value, pvalue.value->reg.type);
         }
         return value;
      }
   };
   using ValueMap = std::unordered_map<spv::Id, SpirVValue>;
   class TypeVoid : public Type {
   public:
      TypeVoid(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeVoid() {}
      virtual bool isVoidType() const override { return true; }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { assert(false); return std::vector<Value *>(); }
   };
   class TypeBool : public Type {
   public:
      TypeBool(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeBool() {}
      virtual bool isBasicType() const override { return true; }
      virtual std::vector<ImmediateValue *> generateBoolConstant(Converter &conv, bool value) const override;
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override { return 1u; } // XXX no idea
      virtual enum DataType getEnumType(int isSigned = -1) const override;
   };
   class TypeInt : public Type {
   public:
      TypeInt(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeInt() {}
      virtual bool isBasicType() const override { return true; }
      virtual std::vector<ImmediateValue *> generateConstant(Converter &conv,
                                                             const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const override;
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override { return static_cast<uint32_t>(width) / 8u; }
      virtual enum DataType getEnumType(int isSigned = -1) const override;

      word width;
      word signedness;
   };
   class TypeFloat : public Type {
   public:
      TypeFloat(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeFloat() {}
      virtual bool isBasicType() const override { return true; }
      virtual std::vector<ImmediateValue *> generateConstant(Converter &conv,
                                                             const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const override;
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override { return static_cast<uint32_t>(width) / 8u; }
      virtual enum DataType getEnumType(int isSigned = -1) const override;

      word width;
   };
   class TypeStruct : public Type {
   public:
      TypeStruct(const spv_parsed_instruction_t *const parsedInstruction, std::unordered_map<spv::Id, Type*> const& types,
                 Decorations const& decorations);
      virtual ~TypeStruct() {}
      virtual bool isCompooundType() const override { return true; }
      virtual std::vector<ImmediateValue *> generateConstant(Converter &conv,
                                                             const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const override;
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override { return size; }
      virtual enum DataType getEnumType(int isSigned = -1) const override;
      virtual unsigned int getElementsNb(void) const override { return static_cast<unsigned>(members.size()); }
      virtual unsigned int getElementSize(unsigned int index) const override;
      virtual Type const* getElementType(unsigned int index) const override;
      virtual enum DataType getElementEnumType(unsigned int index, int isSigned = -1) const override;
      virtual unsigned int getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position = 0u) const override;
      virtual void getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position = 0u) const override;
      virtual std::vector<unsigned int> getPaddings() const override { return memberPaddings; }

      std::vector<Type*> members;
      std::vector<unsigned int> memberPaddings;
      unsigned size;
   };
   class TypeVector : public Type {
   public:
      TypeVector(const spv_parsed_instruction_t *const parsedInstruction, std::unordered_map<spv::Id, Type*> const& types);
      virtual ~TypeVector() {}
      virtual bool isCompooundType() const override { return true; }
      virtual std::vector<ImmediateValue *> generateConstant(Converter &conv,
                                                             const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const override;
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override;
      virtual enum DataType getEnumType(int isSigned = -1) const override;
      virtual unsigned int getElementsNb(void) const override { return static_cast<unsigned>(elementsNb); }
      virtual unsigned int getElementSize(unsigned int /*index*/) const override;
      virtual Type const* getElementType(unsigned int /*index*/) const override;
      virtual enum DataType getElementEnumType(unsigned int /*index*/, int isSigned = -1) const override;
      virtual unsigned int getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position = 0u) const override;
      virtual void getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position = 0u) const override;
      virtual std::vector<unsigned int> getPaddings() const override;
      virtual bool isVectorOfSize(unsigned int size) const override { return size == elementsNb; }

      Type* componentType;
      word elementsNb;
   };
   class TypeArray : public Type {
   public:
      TypeArray(const spv_parsed_instruction_t *const parsedInstruction, std::unordered_map<spv::Id, Type*> const& types,
                 const ValueMap &m);
      virtual ~TypeArray() {}
      virtual bool isCompooundType() const override { return true; }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override;
      virtual enum DataType getEnumType(int isSigned = -1) const override;
      virtual unsigned int getElementsNb(void) const override { return elementsNb; }
      virtual unsigned int getElementSize(unsigned int /*index*/) const override;
      virtual Type const* getElementType(unsigned int /*index*/) const override;
      virtual enum DataType getElementEnumType(unsigned int /*index*/, int isSigned = -1) const override;
      virtual unsigned int getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position = 0u) const override;
      virtual void getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position = 0u) const override;
      virtual std::vector<unsigned int> getPaddings() const override;

      Type* componentType;
      unsigned elementsNb;
   };
   class TypePointer : public Type {
   public:
      TypePointer(const spv_parsed_instruction_t *const parsedInstruction, uint16_t chipset,
                  std::unordered_map<spv::Id, Type*> const& types);
      virtual ~TypePointer() {}
      virtual bool isBasicType() const override { return true; }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override;
      virtual unsigned int getSize(void) const override { return sizeInBytes; }
      virtual enum DataType getEnumType(int isSigned = -1) const override;
      enum SpirvFile getStorageFile() const { return Converter::getStorageFile(storage); }
      Type* getPointedType() const { return type; }
      virtual void getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position = 0u) const override;

      spv::StorageClass storage;
      Type* type;
      unsigned int sizeInBytes;
   };
   class TypeFunction : public Type {
   public:
      TypeFunction(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeFunction() {}
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { assert(false); return std::vector<Value *>(); }

      spv::Id type;
      std::vector<spv::Id> params;
   };
   class TypeSampler : public Type {
   public:
      TypeSampler(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeSampler() {}
      virtual unsigned int getSize(void) const override { return 8u; }
      virtual enum DataType getEnumType(int isSigned = -1) const { return TYPE_U64; }
      enum SpirvFile getStorageFile() const { return SpirvFile::TEMPORARY; }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { assert(false); return std::vector<Value *>(); }

      spv::Id id;
   };
   class TypeImage : public Type {
   public:
      TypeImage(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeImage() {}
      virtual unsigned int getSize(void) const override { return 8u; }
      virtual enum DataType getEnumType(int isSigned = -1) const { return TYPE_U64; }
      enum SpirvFile getStorageFile() const { return SpirvFile::TEMPORARY; }
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { assert(false); return std::vector<Value *>(); }

      spv::Id id;
      spv::Id sampledType;
      spv::Dim dim;
      word depth;
      word arrayed;
      word ms;
      word sampled;
      spv::ImageFormat format;
      spv::AccessQualifier access;
   };
   class TypeSampledImage : public Type {
   public:
      TypeSampledImage(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeSampledImage() {}
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { assert(false); return std::vector<Value *>(); }
      spv::Id getImageType() const { return imageType; }

      spv::Id id;
      spv::Id imageType;
   };
   class TypeEvent : public Type {
   public:
      TypeEvent(const spv_parsed_instruction_t *const parsedInstruction);
      virtual ~TypeEvent() {}
      virtual std::vector<Value *> generateNullConstant(Converter &conv) const override { return { nullptr }; }
   };
   struct Sampler {
      TypeSampler const* type;
      uint32_t index;
      spv::SamplerAddressingMode addressingMode;
      bool normalizedCoords;
      spv::SamplerFilterMode filterMode;
   };
   struct Image {
      TypeImage const* type;
      uint32_t index;
   };
   struct SampledImage {
      TypeSampledImage const* type;
      const Image& image;
      const Sampler& sampler;
   };
   struct FunctionData {
      Function* caller;
      FlowInstruction* callInsn;
      FunctionData(Function* _caller, FlowInstruction* _callInsn)
         : caller(_caller), callInsn(_callInsn) {}
   };

   Converter(Program *, struct nv50_ir_prog_info *info);
   ~Converter();

   bool run();
   spv_result_t convertInstruction(const spv_parsed_instruction_t *parsedInstruction);

private:
   spv_result_t convertEntryPoint(const spv_parsed_instruction_t *parsedInstruction);
   spv_result_t convertDecorate(const spv_parsed_instruction_t *parsedInstruction,
                                bool hasMember = false);
   template<typename T> spv_result_t convertType(const spv_parsed_instruction_t *parsedInstruction);
   Symbol * createSymbol(SpirvFile file, DataType type, unsigned int size, unsigned int offset) {
      Symbol *baseSymbol = baseSymbols[file];
      Symbol *sym = new_Symbol(prog, baseSymbol->reg.file, baseSymbol->reg.fileIndex);
      sym->reg.type = type;
      sym->reg.size = size;

      // TODO(pmoreau): This is a hack to get the proper offset on Tesla
      if (file == SpirvFile::INPUT)
         offset += info->prop.cp.inputOffset;

      sym->setAddress(baseSymbol, offset);

      return sym;
   }
   nv50_ir::operation convertOp(spv::Op op);
   nv50_ir::CondCode convertCc(spv::Op op);
   spv_result_t loadBuiltin(spv::Id dstId, Type const* dstType, Words const& decLiterals, spv::MemoryAccessMask access = spv::MemoryAccessMask::MaskNone);
   spv_result_t convertOpenCLInstruction(spv::Id resId, Type const* type, OpenCLLIB::Entrypoints op, const spv_parsed_instruction_t *parsedInstruction);
   spv_result_t generateCtrlBarrier(spv::Scope executionScope);
   spv_result_t generateMemBarrier(spv::Scope memoryScope, spv::MemorySemanticsMask memorySemantics);
   int getSubOp(spv::Op opcode) const;
   static enum SpirvFile getStorageFile(spv::StorageClass storage);
   static unsigned int getFirstBasicElementSize(Type const* type);
   static enum DataType getFirstBasicElementEnumType(Type const* type);
   static TexTarget getTexTarget(TypeImage const* type);
   static TexInstruction::ImgFormatDesc const* getImageFormat(spv::ImageFormat format);

   Value * acquire(SpirvFile dstFile, Type const* type);
   Value *acquire(SpirvFile file, spv::Id id, Type const* type);
   unsigned load(SpirvFile dstFile, SpirvFile srcFile, spv::Id id, const std::vector<PValue> &ptrs, unsigned int offset, Type const* type, spv::MemoryAccessMask access = spv::MemoryAccessMask::MaskNone, uint32_t alignment = 0u);
   void store(SpirvFile dstFile, PValue const& ptr, unsigned int offset, Value *value, DataType stTy, spv::MemoryAccessMask access, uint32_t alignment);
   void store(SpirvFile dstFile, const std::vector<PValue> &ptrs, unsigned int offset, std::vector<PValue> const& values, Type const* type, spv::MemoryAccessMask access = spv::MemoryAccessMask::MaskNone, uint32_t alignment = 0u);

   struct nv50_ir_prog_info *info;
   const uint32_t *const binary;
   spv::AddressingModel addressingModel;
   spv::MemoryModel memoryModel;
   std::unordered_map<spv::Id, EntryPoint> entryPoints;
   std::unordered_map<spv::Id, std::string> names;
   std::unordered_map<spv::Id, Decoration> decorations;
   std::unordered_map<spv::Id, Type *> types;
   std::unordered_map<spv::Id, Function *> functions;
   std::unordered_map<spv::Id, BasicBlock *> blocks;
   std::unordered_map<spv::Id, std::vector<std::pair<std::vector<PValue>, BasicBlock*>>> phiNodes;
   std::unordered_map<nv50_ir::Instruction*, spv::Id> phiMapping;
   std::unordered_map<spv::Id, std::unordered_map<uint32_t, std::pair<spv::Id, spv::Id>>> phiToMatch;
   std::unordered_map<spv::Id, Sampler> samplers;
   std::unordered_map<spv::Id, Image> images;
   std::unordered_map<spv::Id, SampledImage> sampledImages;

   ValueMap spvValues;

   std::unordered_map<SpirvFile, Symbol *> baseSymbols;
   spv::Id currentFuncId;
   uint32_t inputOffset; // XXX maybe better to have a separate DataArray for input, keeping track
   uint32_t samplerCounter;
   uint32_t imageCounter;

   std::unordered_map<spv::Id, std::vector<FlowInstruction*>> branchesToMatch;
   std::unordered_map<spv::Id, std::vector<FunctionData>> functionsToMatch;
};

class GetOutOfSSA : public Pass
{
public:
   void setData(std::unordered_map<spv::Id, std::vector<std::pair<std::vector<Converter::PValue>, BasicBlock*>>>* nodes, std::unordered_map<nv50_ir::Instruction*, spv::Id>* mapping, Converter::ValueMap* values) {
      phiNodes = nodes;
      phiMapping = mapping;
      spvValues = values;
   }

private:
   virtual bool visit(BasicBlock *);
   bool handlePhi(Instruction *);

   std::unordered_map<spv::Id, std::vector<std::pair<std::vector<Converter::PValue>, BasicBlock*>>>* phiNodes;
   std::unordered_map<nv50_ir::Instruction*, spv::Id>* phiMapping;
   Converter::ValueMap* spvValues;

protected:
   BuildUtil bld;
};

bool
GetOutOfSSA::visit(BasicBlock *bb)
{
   Instruction *next;
   for (Instruction *i = bb->getPhi(); i && i != bb->getEntry(); i = next) {
      next = i->next;
      if (!handlePhi(i)) {
         err = true;
         return false;
      }
   }
   return true;
}

bool
GetOutOfSSA::handlePhi(Instruction *insn)
{
   auto searchId = phiMapping->find(insn);
   if (searchId == phiMapping->end()) {
      _debug_printf("Couldn't find id linked to phi insn:\n\t");
      insn->print();
      return false;
   }
   auto searchData = phiNodes->find(searchId->second);
   if (searchData == phiNodes->end()) {
      _debug_printf("Couldn't find phi node with id %u\n", searchId->second);
      return false;
   }

   const auto& data = searchData->second;
   auto searchValue = spvValues->find(searchId->second);
   if (searchValue == spvValues->end()) {
      _debug_printf("Couldn't find SpirVValue for phi node with id %u\n", searchId->second);
      return false;
   }

   for (auto& pair : data) {
      if (pair.first.size() > 1u)
         _debug_printf("Multiple var for same phi node aren't really supported\n");
      auto bbExit = pair.second->getExit();
      if (bbExit == nullptr) {
         _debug_printf("BB.exit == nullptr; this is unexpected, things will go wrong!\n");
         return false;
      }
      bld.setPosition(bbExit, !(bbExit->op == OP_BRA || bbExit->op == OP_EXIT));
      bld.mkMov(searchValue->second.value[0].value, pair.first[0].value, searchValue->second.type->getEnumType());
   }

   delete_Instruction(bld.getProgram(), insn);

   return true;
}

template<typename T>
static
ImmediateValue* generateImmediate(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex)
{
   T value = spirv::getOperand<T>(parsedInstruction, operandIndex);
   return conv.mkImm(value);
}

Converter::TypeVoid::TypeVoid(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeVoid)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
}

Converter::TypeBool::TypeBool(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeBool)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   alignment = 1u;
}

std::vector<ImmediateValue *>
Converter::TypeBool::generateBoolConstant(Converter &conv, bool value) const
{
   return { value ? conv.mkImm(1u) : conv.mkImm(0u) };
}

std::vector<Value *>
Converter::TypeBool::generateNullConstant(Converter &conv) const
{
   return { conv.mkImm(0u) };
}

enum DataType
Converter::TypeBool::getEnumType(int /*isSigned*/) const
{
   return DataType::TYPE_NONE;
}

Converter::TypeInt::TypeInt(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeInt)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   width = spirv::getOperand<unsigned>(parsedInstruction, 1u);
   signedness = spirv::getOperand<unsigned>(parsedInstruction, 2u);
   alignment = width / 8u;
}

std::vector<ImmediateValue *>
Converter::TypeInt::generateConstant(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const
{
   ImmediateValue *imm = nullptr;
   DataType type = getEnumType();
   switch (type) {
   case DataType::TYPE_S8: // FALLTHROUGH
   case DataType::TYPE_U8:
      imm = generateImmediate<uint8_t>(conv, parsedInstruction, operandIndex);
      break;
   case DataType::TYPE_S16: // FALLTHROUGH
   case DataType::TYPE_U16:
      imm = generateImmediate<uint16_t>(conv, parsedInstruction, operandIndex);
      break;
   case DataType::TYPE_S32: // FALLTHROUGH
   case DataType::TYPE_U32:
      imm = generateImmediate<uint32_t>(conv, parsedInstruction, operandIndex);
      break;
   case DataType::TYPE_S64: // FALLTHROUGH
   case DataType::TYPE_U64:
      imm = generateImmediate<uint64_t>(conv, parsedInstruction, operandIndex);
      break;
   default:
      assert(false);
      return { nullptr };
   }
   imm->reg.type = type;
   ++operandIndex;
   return { imm };
}

// TODO(pmoreau): Might need to be fixed
std::vector<Value *>
Converter::TypeInt::generateNullConstant(Converter &conv) const
{
   return { (width == 64u) ? conv.mkImm(0ul) : conv.mkImm(0u) };
}

enum DataType
Converter::TypeInt::getEnumType(int isSigned) const
{
   if (isSigned == 1 || (isSigned == -1 && signedness == 1u)) {
      if (width == 8u)
         return DataType::TYPE_S8;
      else if (width == 16u)
         return DataType::TYPE_S16;
      else if (width == 32u)
         return DataType::TYPE_S32;
      else if (width == 64u)
         return DataType::TYPE_S64;
      else {
         assert(false);
         return DataType::TYPE_NONE;
      }
   }
   if (isSigned == 0 || (isSigned == -1 && signedness == 0u)) {
      if (width == 8u)
         return DataType::TYPE_U8;
      else if (width == 16u)
         return DataType::TYPE_U16;
      else if (width == 32u)
         return DataType::TYPE_U32;
      else if (width == 64u)
         return DataType::TYPE_U64;
      else {
         assert(false);
         return DataType::TYPE_NONE;
      }
   }
   return DataType::TYPE_NONE;
}

Converter::TypeFloat::TypeFloat(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeFloat)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   width = spirv::getOperand<unsigned>(parsedInstruction, 1u);
   alignment = width / 8u;
}

std::vector<ImmediateValue *>
Converter::TypeFloat::generateConstant(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const
{
   ImmediateValue *imm = nullptr;
   DataType type = getEnumType();
   switch (type) {
   case DataType::TYPE_F32:
      imm = generateImmediate<float>(conv, parsedInstruction, operandIndex);
      break;
   case DataType::TYPE_F64:
      imm = generateImmediate<double>(conv, parsedInstruction, operandIndex);
      break;
   default:
      _debug_printf("Unsupported floating point type.\n");
      assert(false);
      return { nullptr };
   }
   imm->reg.type = type;
   ++operandIndex;
   return { imm };

}

std::vector<Value *>
Converter::TypeFloat::generateNullConstant(Converter &conv) const
{
   return { (width == 64u) ? conv.mkImm(0.0) : conv.mkImm(0.0f) };
}

enum DataType
Converter::TypeFloat::getEnumType(int /*isSigned*/) const
{
   if (width == 16u)
      return DataType::TYPE_F16;
   else if (width == 32u)
      return DataType::TYPE_F32;
   else if (width == 64u)
      return DataType::TYPE_F64;
   else {
      assert(false);
      return DataType::TYPE_NONE;
   }
}

Converter::TypeStruct::TypeStruct(const spv_parsed_instruction_t *const parsedInstruction,
                                  std::unordered_map<spv::Id, Type*> const& types,
                                  std::unordered_map<spv::Id, Decoration> const& decorations) : Type(spv::Op::OpTypeStruct)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   size = 0u;
   members.reserve(parsedInstruction->num_operands - 1u);
   auto largestAlignment = 0u;

   bool isPacked = false;
   const auto &decos = decorations.find(id);
   if (decos != decorations.end())
      isPacked = decos->second.find(spv::Decoration::CPacked) != decos->second.end();

   for (unsigned int i = 1u; i < parsedInstruction->num_operands; ++i) {
      const auto memberId = spirv::getOperand<spv::Id>(parsedInstruction, i);
      auto search = types.find(memberId);
      assert(search != types.end());

      members.push_back(search->second);

      const auto memberSize = search->second->getSize();
      const auto memberAlignment = isPacked ? 1u : search->second->getAlignment();
      largestAlignment = std::max(largestAlignment, memberAlignment);
      const auto padding = (-size) & (memberAlignment - 1u);
      size += padding + memberSize;

      if (search->second->isCompooundType()) {
         auto paddings = search->second->getPaddings();
         paddings[0] += padding;
         memberPaddings.insert(memberPaddings.end(), paddings.begin(), paddings.end());
      } else {
         memberPaddings.push_back(padding);
      }
   }
   size += (-size) & (largestAlignment - 1u);
   alignment = largestAlignment;
}

std::vector<ImmediateValue *>
Converter::TypeStruct::generateConstant(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const
{
   std::vector<ImmediateValue *> imms;
   for (const Type *member : members) {
      const auto constants = member->generateConstant(conv, parsedInstruction, operandIndex);
      imms.insert(imms.end(), constants.begin(), constants.end());
   }
   return imms;
}

std::vector<Value *>
Converter::TypeStruct::generateNullConstant(Converter &conv) const
{
   std::vector<Value *> nullConstant;
   for (const Type *member : members) {
      const auto constants = member->generateNullConstant(conv);
      nullConstant.insert(nullConstant.end(), constants.begin(), constants.end());
   }
   return nullConstant;
}

enum DataType
Converter::TypeStruct::getEnumType(int /*isSigned*/) const
{
   return DataType::TYPE_NONE;
}

unsigned int
Converter::TypeStruct::getElementSize(unsigned int index) const
{
   assert(index < members.size());
   assert(members[index] != nullptr);
   return members[index]->getSize();
}

Converter::Type const*
Converter::TypeStruct::getElementType(unsigned int index) const
{
   assert(index < members.size());
   assert(members[index] != nullptr);
   return members[index];
}

enum DataType
Converter::TypeStruct::getElementEnumType(unsigned int index, int isSigned) const
{
   assert(index < members.size());
   assert(members[index] != nullptr);
   return members[index]->getEnumType(isSigned);
}

// TODO(pmoreau): This seems wrong
unsigned int
Converter::TypeStruct::getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position) const
{
   assert(position == elementIds.size() - 1u);
   return elementIds[position];
}

// TODO(pmoreau): This seems wrong
void
Converter::TypeStruct::getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position) const
{
   assert(position < ids.size());

   const auto imm = ids[position];
   uint32_t structOff = 0u;
   for (int i = 0; i < imm->reg.data.u32; ++i)
      structOff += members[i]->getSize();
   auto res = bu->getScratch(offset->reg.size);
   if (offset->reg.type == TYPE_U64)
      bu->loadImm(res, static_cast<unsigned long>(structOff));
   else
      bu->loadImm(res, structOff);
   bu->mkOp2(OP_ADD, offset->reg.type, offset, offset, res);

   if (position + 1u < ids.size())
      assert(false);
}

Converter::TypeVector::TypeVector(const spv_parsed_instruction_t *const parsedInstruction,
                                  std::unordered_map<spv::Id, Type*> const& types) : Type(spv::Op::OpTypeVector)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   spv::Id const componentTypeId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
   auto search = types.find(componentTypeId);
   assert(search != types.end());

   componentType = search->second;
   elementsNb = spirv::getOperand<unsigned>(parsedInstruction, 2u);
   alignment = (elementsNb != 3u) ? elementsNb * componentType->getSize() : 4u * componentType->getSize();
}

// TODO(pmoreau): check this one, this does not seem correct.
std::vector<ImmediateValue *>
Converter::TypeVector::generateConstant(Converter &conv, const spv_parsed_instruction_t *parsedInstruction, uint16_t &operandIndex) const
{
   std::vector<ImmediateValue *> immsConstant;
   const auto memberConstant = componentType->generateConstant(conv, parsedInstruction, operandIndex);
   for (unsigned int i = 0u; i < elementsNb; ++i)
      immsConstant.insert(immsConstant.end(), memberConstant.begin(), memberConstant.end());
   return immsConstant;
}

std::vector<Value *>
Converter::TypeVector::generateNullConstant(Converter &conv) const
{
   std::vector<Value *> nullConstant;
   const auto memberConstant = componentType->generateNullConstant(conv);
   for (unsigned int i = 0u; i < elementsNb; ++i)
      nullConstant.insert(nullConstant.end(), memberConstant.begin(), memberConstant.end());
   return nullConstant;
}

unsigned int
Converter::TypeVector::getSize(void) const
{
   assert(componentType != nullptr);
   return componentType->getSize() * ((elementsNb != 3u) ? elementsNb : 4u);
}

Converter::Type const*
Converter::TypeVector::getElementType(unsigned int /*index*/) const
{
   assert(componentType != nullptr);
   return componentType;
}

enum DataType
Converter::TypeVector::getEnumType(int /*isSigned*/) const
{
   return DataType::TYPE_NONE;
}

unsigned int
Converter::TypeVector::getElementSize(unsigned int /*index*/) const
{
   assert(componentType != nullptr);
   return componentType->getSize();
}

enum DataType
Converter::TypeVector::getElementEnumType(unsigned int /*index*/, int isSigned) const
{
   assert(componentType != nullptr);
   return componentType->getEnumType(isSigned);
}


unsigned int
Converter::TypeVector::getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position) const
{
   assert(position == elementIds.size() - 1u);
   return elementIds[position];
}

void
Converter::TypeVector::getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position) const
{
   assert(componentType != nullptr && position < ids.size());

   auto res = bu->getScratch(offset->reg.size);
   if (offset->reg.type == TYPE_U64)
      bu->loadImm(res, static_cast<unsigned long>(componentType->getSize()));
   else
      bu->loadImm(res, componentType->getSize());
   Value *index = bu->getScratch(offset->reg.size);
   bu->mkMov(index, ids[position], offset->reg.type);
   bu->mkOp3(OP_MAD, offset->reg.type, offset, index, res, offset);

   if (position + 1u < ids.size())
      assert(false);
}

std::vector<unsigned int>
Converter::TypeVector::getPaddings() const
{
   std::vector<unsigned int> paddings;
   const auto elementPaddings = componentType->getPaddings();
   for (unsigned int i = 0u; i < elementsNb; ++i)
      paddings.insert(paddings.end(), elementPaddings.cbegin(), elementPaddings.cend());
   return paddings;
}

Converter::TypeArray::TypeArray(const spv_parsed_instruction_t *const parsedInstruction,
                                std::unordered_map<spv::Id, Type*> const& types,
                                const ValueMap &m) : Type(spv::Op::OpTypeArray)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   spv::Id const componentTypeId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
   auto search = types.find(componentTypeId);
   assert(search != types.end());

   componentType = search->second;
   spv::Id const elementsNbId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);
   auto searchElemNb = m.find(elementsNbId);
   assert(searchElemNb != m.end() && searchElemNb->second.storageFile == SpirvFile::IMMEDIATE);
   elementsNb = searchElemNb->second.value.front().value->asImm()->reg.data.u32;
   alignment = componentType->getAlignment();
}

std::vector<Value *>
Converter::TypeArray::generateNullConstant(Converter &conv) const
{
   std::vector<Value *> nullConstant;
   const auto memberConstant = componentType->generateNullConstant(conv);
   for (unsigned int i = 0u; i < elementsNb; ++i)
      nullConstant.insert(nullConstant.end(), memberConstant.begin(), memberConstant.end());
   return nullConstant;
}

Converter::Type const*
Converter::TypeArray::getElementType(unsigned int /*index*/) const
{
   assert(componentType != nullptr);
   return componentType;
}

unsigned int
Converter::TypeArray::getSize(void) const
{
   assert(componentType != nullptr);
   assert(elementsNb != 0u);
   return componentType->getSize() * elementsNb;
}

enum DataType
Converter::TypeArray::getEnumType(int /*isSigned*/) const
{
   return DataType::TYPE_NONE;
}

unsigned int
Converter::TypeArray::getElementSize(unsigned int /*index*/) const
{
   assert(componentType != nullptr);
   return componentType->getSize();
}

enum DataType
Converter::TypeArray::getElementEnumType(unsigned int /*index*/, int isSigned) const
{
   assert(componentType != nullptr);
   return componentType->getEnumType(isSigned);
}


unsigned int
Converter::TypeArray::getGlobalIdx(std::vector<unsigned int> const& elementIds, unsigned position) const
{
   assert(position == elementIds.size() - 1u);
   return elementIds[position];
}

void
Converter::TypeArray::getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position) const
{
   assert(componentType != nullptr && position < ids.size());

   auto res = bu->getScratch(offset->reg.size);
   if (offset->reg.type == TYPE_U64)
      bu->loadImm(res, static_cast<unsigned long>(componentType->getSize()));
   else
      bu->loadImm(res, componentType->getSize());
   Value *index = bu->getScratch(offset->reg.size);
   bu->mkMov(index, ids[position], offset->reg.type);
   bu->mkOp3(OP_MAD, offset->reg.type, offset, index, res, offset);

   componentType->getGlobalOffset(bu, decoration, offset, ids, position + 1u);
}

std::vector<unsigned int>
Converter::TypeArray::getPaddings() const
{
   std::vector<unsigned int> paddings;
   const auto elementPaddings = componentType->getPaddings();
   for (unsigned int i = 0u; i < elementsNb; ++i)
      paddings.insert(paddings.end(), elementPaddings.begin(), elementPaddings.end());
   return paddings;
}

Converter::TypePointer::TypePointer(const spv_parsed_instruction_t *const parsedInstruction,
                                    uint16_t chipset,
                                    std::unordered_map<spv::Id, Type*> const& types) : Type(spv::Op::OpTypePointer)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   storage = spirv::getOperand<spv::StorageClass>(parsedInstruction, 1u);
   auto const typeId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);
   auto search = types.find(typeId);
   assert(search != types.end());

   type = search->second;
   sizeInBytes = (chipset >= 0xc0) ? 8u : 4u;
   alignment = sizeInBytes;
}

std::vector<Value *>
Converter::TypePointer::generateNullConstant(Converter &conv) const
{
   return { (sizeInBytes == 8u) ? conv.mkImm(0ul) : conv.mkImm(0u) };
}

enum DataType
Converter::TypePointer::getEnumType(int /*isSigned*/) const
{
   if (sizeInBytes == 8u)
      return DataType::TYPE_U64;
   else
      return DataType::TYPE_U32;
}

// TODO(pmoreau): Check this function again, it does not seem to be correct.
void
Converter::TypePointer::getGlobalOffset(BuildUtil *bu, Decoration const& decoration, Value *offset, std::vector<Value *> ids, unsigned position) const
{
   assert(position < ids.size());

   if (storage != spv::StorageClass::Function) {
      unsigned int typeSize = type->getSize();

      auto searchAlignment = decoration.find(spv::Decoration::Alignment);
      if (searchAlignment != decoration.end())
         typeSize += (-typeSize) & (searchAlignment->second[0][0] - 1u);

      Value *tmp = bu->getScratch(offset->reg.size);
      if (offset->reg.type == TYPE_U64)
         bu->loadImm(tmp, static_cast<unsigned long>(typeSize));
      else
         bu->loadImm(tmp, typeSize);
      Value *index = bu->getScratch(offset->reg.size);
      bu->mkMov(index, ids[position], offset->reg.type);
      bu->mkOp3(OP_MAD, offset->reg.type, offset, tmp, index, offset);
   } else {
      assert(ids[position]->asImm() != nullptr && ids[position]->asImm()->reg.data.u64 == 0ul);
   }

   if (position + 1u < ids.size())
      type->getGlobalOffset(bu, decoration, offset, ids, position + 1u);
}

Converter::TypeFunction::TypeFunction(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeFunction)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   type = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
   for (unsigned int i = 2u; i < parsedInstruction->num_operands; ++i)
      params.push_back(spirv::getOperand<spv::Id>(parsedInstruction, i));
   alignment = 0u;
}

Converter::TypeSampler::TypeSampler(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeSampler)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   alignment = 0u;
}

Converter::TypeSampledImage::TypeSampledImage(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeSampledImage)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   imageType = spirv::getOperand<spv::Id>(parsedInstruction, 1);
   alignment = 0u;
}

Converter::TypeImage::TypeImage(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeImage)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
   sampledType = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
   dim = spirv::getOperand<spv::Dim>(parsedInstruction, 2u);
   depth = spirv::getOperand<unsigned>(parsedInstruction, 3u);
   arrayed = spirv::getOperand<unsigned>(parsedInstruction, 4u);
   ms = spirv::getOperand<unsigned>(parsedInstruction, 5u);
   sampled = spirv::getOperand<unsigned>(parsedInstruction, 6u);
   format = spirv::getOperand<spv::ImageFormat>(parsedInstruction, 7u);
   if (parsedInstruction->num_operands == 9u)
      access = spirv::getOperand<spv::AccessQualifier>(parsedInstruction, 8u);
   alignment = 0u;
}

Converter::TypeEvent::TypeEvent(const spv_parsed_instruction_t *const parsedInstruction) : Type(spv::Op::OpTypeEvent)
{
   id = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
}

Value *
Converter::acquire(SpirvFile dstFile, Type const* type)
{
   assert(type != nullptr);

   if (dstFile == SpirvFile::TEMPORARY) {
      Value *res = nullptr; // FIXME still meh
      if (getFunction()) {
         res = getScratch(std::max(4u, type->getSize()));
         res->reg.type = type->getEnumType();
      }
      return res;
   }

   return createSymbol(dstFile, getFirstBasicElementEnumType(type), std::max(4u, getFirstBasicElementSize(type)), 0u);
}

Value *
Converter::acquire(SpirvFile file, spv::Id id, Type const* type)
{
   assert(type != nullptr);

   auto values = std::vector<PValue>();
   Value *res = nullptr;

   auto saveToShare = file == SpirvFile::SHARED;

   const Type *processedType = type;
   std::stack<const Type *> types;
   auto ptrType = reinterpret_cast<const TypePointer *>(type);
   if (type->getType() == spv::Op::OpTypePointer && ptrType->getStorageFile() == SpirvFile::TEMPORARY)
      processedType = ptrType->getPointedType();
   types.push(processedType);

   while (!types.empty()) {
      const Type *currentType = types.top();
      types.pop();

      if (currentType->isCompooundType()) {
         for (unsigned int i = currentType->getElementsNb(); i > 0u; --i)
            types.push(currentType->getElementType(i - 1u));
         continue;
      }

      res = acquire(file, currentType);
      if (res->reg.file == FILE_GPR)
         values.push_back(res);
      else
         values.emplace_back(res->asSym(), nullptr);
      if (saveToShare)
         info->bin.smemSize += currentType->getSize();
   }

   spvValues.emplace(id, SpirVValue{ file, type, values, processedType->getPaddings() });

   return res;
}

// TODO(pmoreau):
// * Make sure to handle all alignment/padding/weird cases properly
// * Handle all different MemoryAccess
// * Handle loads from one memory space to another one?
// * loads should?/could be emited with a size of alignment.
unsigned
Converter::load(SpirvFile dstFile, SpirvFile srcFile, spv::Id id, const std::vector<PValue> &ptrs, unsigned int offset, Type const* type, spv::MemoryAccessMask access, uint32_t alignment)
{
   assert(type != nullptr);

   std::vector<PValue> values;

   const bool hasLoadAlignment = hasFlag(access, spv::MemoryAccessShift::Aligned);
   std::uint32_t localOffset = offset;

   std::stack<Type const*> stack;
   stack.push(type);

   while (!stack.empty()) {
      const Type *currentType = stack.top();
      stack.pop();

      if (!currentType->isCompooundType()) {
         const std::uint32_t elemByteSize = currentType->getSize();
         const std::uint32_t elemBitSize = elemByteSize * 8u;

         const std::uint32_t typeAlignment = !hasLoadAlignment ? elemByteSize : alignment;

         // Pad the current offset, if needed, in order to have this new access
         // correctly aligned to the custom alignment, if provided, or the
         // type’s size.
         const std::uint32_t alignmentDelta = localOffset % typeAlignment;
         if (alignmentDelta != 0u)
            localOffset += typeAlignment - alignmentDelta;
         assert(typeAlignment >= elemByteSize);

         const std::uint32_t destByteSize = std::max(4u, elemByteSize);
         const PValue &ptrTmp = ptrs[0u];
         const bool srcInGPR = srcFile == SpirvFile::IMMEDIATE || srcFile == SpirvFile::TEMPORARY ||
                               (ptrTmp.indirect != nullptr && ptrTmp.indirect->reg.file == FILE_IMMEDIATE);

         const DataType destEnumType = typeOfSize(destByteSize);
         const DataType elemEnumType = currentType->getEnumType();
         Value *res = getScratch(destByteSize);
         res->reg.type = elemEnumType; // Might not be needed; could be used to track false U8 values.

         Instruction *insn = nullptr;
         if (srcInGPR) {
            const std::uint32_t c = static_cast<std::uint32_t>(values.size());
            assert(c < ptrs.size());
            const PValue &ptr = ptrs[c];
            insn = mkMov(res, ptr.indirect, destEnumType);
         } else {
            const PValue &ptr = ptrs[0u];
            Symbol *sym = ptr.symbol;
            if (sym == nullptr)
               sym = createSymbol(srcFile, elemEnumType, elemByteSize, localOffset);

            insn = mkLoad(elemEnumType, res, sym, ptr.indirect);
         }
         if (hasFlag(access, spv::MemoryAccessShift::Volatile))
            insn->fixed = 1;

         localOffset += elemByteSize;
         values.push_back(res);
      } else {
         for (unsigned int i = currentType->getElementsNb(); i != 0u; --i)
            stack.push(currentType->getElementType(i - 1u));
      }
   }

   spvValues.emplace(id, SpirVValue{ dstFile, type, values, type->getPaddings() });

   return localOffset - offset;
}

// TODO use access
void
Converter::store(SpirvFile dstFile, PValue const& ptr, unsigned int offset, Value *value, DataType stTy, spv::MemoryAccessMask access, uint32_t alignment)
{
   assert(value != nullptr);

   Value *realValue = value;
   if (value->reg.file == FILE_IMMEDIATE) {
      realValue = getScratch(value->reg.size);
      Instruction *insn = mkMov(realValue, value, typeOfSize(value->reg.size));
      if (hasFlag(access, spv::MemoryAccessShift::Volatile))
         insn->fixed = 1;
   }

   if (dstFile == SpirvFile::TEMPORARY) {
      Instruction *insn = mkMov(ptr.indirect, realValue, typeOfSize(value->reg.size));
      if (hasFlag(access, spv::MemoryAccessShift::Volatile))
         insn->fixed = 1;
      return;
   }

   Symbol *sym = ptr.symbol;
   if (sym == nullptr)
      sym = createSymbol(dstFile, realValue->reg.type, realValue->reg.size, offset);

   // TODO(pmoreau): This is a hack to get the proper offset on Tesla
   Value *tmp = nullptr;
   if (info->target >= 0xc0)
      tmp = ptr.indirect;
   else
      tmp = mkOp2v(OP_ADD, ptr.indirect->reg.type, getScratch(ptr.indirect->reg.size), ptr.indirect, loadImm(NULL, offset));
   Instruction* insn = mkStore(OP_STORE, stTy, sym, tmp, realValue);
   if (hasFlag(access, spv::MemoryAccessShift::Volatile))
      insn->fixed = 1;
}

// TODO(pmoreau):
// * stores should?/could be emited with a size of alignment.
void
Converter::store(SpirvFile dstFile, const std::vector<PValue> &ptrs, unsigned int offset, std::vector<PValue> const& values, Type const* type, spv::MemoryAccessMask access, uint32_t alignment)
{
   assert(type != nullptr);

   const bool hasStoreAlignment = hasFlag(access, spv::MemoryAccessShift::Aligned);
   std::uint32_t localOffset = offset;
   std::uint32_t c = 0u;

   std::stack<Type const*> stack;
   stack.push(type);

   while (!stack.empty()) {
      auto currentType = stack.top();
      stack.pop();

      if (!currentType->isCompooundType()) {
         const std::uint32_t elemByteSize = currentType->getSize();
         const std::uint32_t dstByteSize = std::max(4u, elemByteSize);

         const std::uint32_t typeAlignment = !hasStoreAlignment ? elemByteSize : alignment;

         // Pad the current offset, if needed, in order to have this new access
         // correctly aligned to the custom alignment, if provided, or the
         // type’s size.
         const std::uint32_t alignmentDelta = localOffset % typeAlignment;
         if (alignmentDelta != 0u)
            localOffset += typeAlignment - alignmentDelta;
         assert(typeAlignment >= elemByteSize);

         assert(c <= values.size());
         Value *value = values[c].value;

         const DataType elemEnumType = currentType->getEnumType();
         const DataType dstEnumType = typeOfSize(dstByteSize);

         // If we have an immediate as input, move it first into a register.
         if (value->reg.file == FILE_IMMEDIATE) {
            Value *immValue = getScratch(dstByteSize);
            mkMov(immValue, value, dstEnumType);
            value = immValue;
         }

         Instruction *insn = nullptr;
         if (dstFile == SpirvFile::TEMPORARY) {
            assert(c < ptrs.size());
            const PValue &ptr = ptrs[c];
            insn = mkMov(ptr.indirect, value, dstEnumType);
         } else {
            const PValue &ptr = ptrs[0u];
            Symbol *sym = ptr.symbol;
            if (sym == nullptr)
               sym = createSymbol(dstFile, elemEnumType, elemByteSize, localOffset);

            // TODO(pmoreau): This is a hack to get the proper offset on Tesla
            Value *tmp = nullptr;
            if (info->target >= 0xc0)
               tmp = ptr.indirect;
            else
               tmp = mkOp2v(OP_ADD, ptr.indirect->reg.type, getScratch(ptr.indirect->reg.size), ptr.indirect, loadImm(nullptr, localOffset));
            insn = mkStore(OP_STORE, elemEnumType, sym, tmp, value);
         }
         if (hasFlag(access, spv::MemoryAccessShift::Volatile))
            insn->fixed = 1;

         localOffset += elemByteSize;
         ++c;
      } else {
         for (unsigned int i = currentType->getElementsNb(); i != 0u; --i)
            stack.push(currentType->getElementType(i - 1u));
      }
   }
}

Converter::Converter(Program *prog, struct nv50_ir_prog_info *info) : BuildUtil(prog),
   info(info), binary(reinterpret_cast<const uint32_t *const>(info->bin.source)),
   addressingModel(), memoryModel(), entryPoints(), decorations(), types(),
   functions(), blocks(), phiNodes(), phiMapping(), phiToMatch(),
   samplers(), sampledImages(), spvValues(), currentFuncId(0u),
   inputOffset(0u), branchesToMatch(), functionsToMatch()
{
   baseSymbols[SpirvFile::TEMPORARY] = new_Symbol(prog, FILE_GPR);
   baseSymbols[SpirvFile::SHARED]    = new_Symbol(prog, FILE_MEMORY_SHARED);
   baseSymbols[SpirvFile::GLOBAL]    = new_Symbol(prog, FILE_MEMORY_GLOBAL, 15);
   baseSymbols[SpirvFile::CONST]     = new_Symbol(prog, FILE_MEMORY_CONST);
   baseSymbols[SpirvFile::PREDICATE] = new_Symbol(prog, FILE_PREDICATE);

   if (info->target >= 0xc0) {
      baseSymbols[SpirvFile::SHARED]->setOffset(info->prop.cp.sharedOffset);
      baseSymbols[SpirvFile::CONST]->setOffset(info->prop.cp.inputOffset);
      baseSymbols[SpirvFile::INPUT] = baseSymbols[SpirvFile::CONST];
   } else {
      baseSymbols[SpirvFile::SHARED]->setOffset(info->prop.cp.inputOffset);
      baseSymbols[SpirvFile::INPUT] = baseSymbols[SpirvFile::SHARED];
   }
}

Converter::~Converter()
{
   for (auto &i : types)
      delete i.second;
}

Converter::SpirvFile
Converter::getStorageFile(spv::StorageClass storage)
{
   switch (storage) {
   case spv::StorageClass::UniformConstant:
      return SpirvFile::CONST;
   case spv::StorageClass::Input:
      return SpirvFile::INPUT;
   case spv::StorageClass::Workgroup:
      return SpirvFile::SHARED;
   case spv::StorageClass::CrossWorkgroup:
      return SpirvFile::GLOBAL;
   case spv::StorageClass::Function:
      return SpirvFile::TEMPORARY;
   case spv::StorageClass::Generic: // FALLTHROUGH
   case spv::StorageClass::AtomicCounter: // FALLTHROUGH
   case spv::StorageClass::Image: // FALLTHROUGH
   default:
      debug_printf("StorageClass %u isn't supported yet\n", storage);
      assert(false);
      return SpirvFile::NONE;
   }
}

unsigned int
Converter::getFirstBasicElementSize(Type const* type)
{
   Type const* currType = type;
   while (!currType->isBasicType())
      currType = currType->getElementType(0u);
   return currType->getSize();
}

enum DataType
Converter::getFirstBasicElementEnumType(Type const* type)
{
   Type const* currType = type;
   while (!currType->isBasicType())
      currType = currType->getElementType(0u);
   return currType->getEnumType();
}

static spv_result_t
handleInstruction(void *userData, const spv_parsed_instruction_t *parsedInstruction)
{
   return reinterpret_cast<Converter*>(userData)->convertInstruction(parsedInstruction);
}

bool
Converter::run()
{
   if (info->dbgFlags)
      pipe_debug_message(info->debug, SHADER_INFO,
                         "Compiling for nv%02x\n", info->target);

   // TODO try to remove/get around that main function
   BasicBlock *entry = new BasicBlock(prog->main);
   prog->main->setEntry(entry);
   prog->main->setExit(new BasicBlock(prog->main));

   const unsigned int numWords = info->bin.sourceLength / 4u;

   spv_context context = spvContextCreate(SPV_ENV_OPENCL_1_2);
   spv_diagnostic diag = nullptr;
   const spv_result_t res = spvBinaryParse(context, this, binary, numWords,
         nullptr, handleInstruction, &diag);
   if (res != SPV_SUCCESS) {
      _debug_printf("Failed to parse the SPIR-V binary:\n");
      spvDiagnosticPrint(diag);
      spvDiagnosticDestroy(diag);
      spvContextDestroy(context);
      return false;
   }
   spvDiagnosticDestroy(diag);
   spvContextDestroy(context);

   for (auto& i : functionsToMatch) {
      auto funcIter = functions.find(i.first);
      if (funcIter == functions.end()) {
         _debug_printf("Unable to find function %u\n", i.first);
         return false;
      }
      auto f = funcIter->second;
      for (auto& j : i.second) {
         j.callInsn->target.fn = f;
         j.caller->call.attach(&f->call, Graph::Edge::TREE);
      }
   }
   functionsToMatch.clear();

   GetOutOfSSA outOfSSAPass;
   outOfSSAPass.setData(&phiNodes, &phiMapping, &spvValues);
   if (!outOfSSAPass.run(prog, true, false))
      return false;

   return true;
}

template<typename T> spv_result_t
Converter::convertType(const spv_parsed_instruction_t *parsedInstruction)
{
   T *type = new T(parsedInstruction);
   types.emplace(type->id, type);

   return SPV_SUCCESS;
}

template<> spv_result_t
Converter::convertType<Converter::TypeStruct>(const spv_parsed_instruction_t *parsedInstruction)
{
   auto *type = new TypeStruct(parsedInstruction, types, decorations);
   types.emplace(type->id, type);

   return SPV_SUCCESS;
}

template<> spv_result_t
Converter::convertType<Converter::TypeVector>(const spv_parsed_instruction_t *parsedInstruction)
{
   auto *type = new TypeVector(parsedInstruction, types);
   types.emplace(type->id, type);

   return SPV_SUCCESS;
}

template<> spv_result_t
Converter::convertType<Converter::TypeArray>(const spv_parsed_instruction_t *parsedInstruction)
{
   auto *type = new TypeArray(parsedInstruction, types, spvValues);
   types.emplace(type->id, type);

   return SPV_SUCCESS;
}

template<> spv_result_t
Converter::convertType<Converter::TypePointer>(const spv_parsed_instruction_t *parsedInstruction)
{
   auto *type = new TypePointer(parsedInstruction, info->target, types);
   types.emplace(type->id, type);

   return SPV_SUCCESS;
}

nv50_ir::operation
Converter::convertOp(spv::Op op)
{
   switch (op) {
   case spv::Op::OpSNegate:
   case spv::Op::OpFNegate:
      return OP_NEG;
   case spv::Op::OpIAdd:
   case spv::Op::OpFAdd:
      return OP_ADD;
   case spv::Op::OpISub:
   case spv::Op::OpFSub:
      return OP_SUB;
   case spv::Op::OpIMul:
   case spv::Op::OpFMul:
      return OP_MUL;
   case spv::Op::OpSDiv:
   case spv::Op::OpUDiv:
   case spv::Op::OpFDiv:
      return OP_DIV;
   case spv::Op::OpSMod:
   case spv::Op::OpUMod:
   case spv::Op::OpFMod:
      return OP_MOD;
   case spv::Op::OpShiftLeftLogical:
      return OP_SHL;
   case spv::Op::OpShiftRightLogical:
   case spv::Op::OpShiftRightArithmetic:
      return OP_SHR;
   case spv::Op::OpBitwiseOr:
      return OP_OR;
   case spv::Op::OpBitwiseXor:
      return OP_XOR;
   case spv::Op::OpBitwiseAnd:
      return OP_AND;
   default:
      return OP_NOP;
   }
}

nv50_ir::CondCode
Converter::convertCc(spv::Op op)
{
   switch (op) {
   case spv::Op::OpIEqual:
   case spv::Op::OpFOrdEqual:
      return CC_EQ;
   case spv::Op::OpINotEqual:
   case spv::Op::OpFOrdNotEqual:
      return CC_NE;
   case spv::Op::OpSGreaterThan:
   case spv::Op::OpUGreaterThan:
   case spv::Op::OpFOrdGreaterThan:
      return CC_GT;
   case spv::Op::OpFUnordGreaterThan:
      return CC_GTU;
   case spv::Op::OpSGreaterThanEqual:
   case spv::Op::OpUGreaterThanEqual:
   case spv::Op::OpFOrdGreaterThanEqual:
      return CC_GE;
   case spv::Op::OpFUnordGreaterThanEqual:
      return CC_GEU;
   case spv::Op::OpSLessThan:
   case spv::Op::OpULessThan:
   case spv::Op::OpFOrdLessThan:
      return CC_LT;
   case spv::Op::OpFUnordLessThan:
      return CC_LTU;
   case spv::Op::OpSLessThanEqual:
   case spv::Op::OpULessThanEqual:
   case spv::Op::OpFOrdLessThanEqual:
      return CC_LE;
   case spv::Op::OpFUnordLessThanEqual:
      return CC_LEU;
   default:
      return CC_NO;
   }
}

spv_result_t
Converter::generateCtrlBarrier(spv::Scope executionScope)
{
   if (executionScope != spv::Scope::Subgroup && executionScope != spv::Scope::Workgroup) {
      _debug_printf("Only subgroup and workgroup scopes are currently supported.\n");
      return SPV_ERROR_INVALID_BINARY;
   }

   Instruction *insn = mkOp2(OP_BAR, TYPE_U32, nullptr, mkImm(0), mkImm(0));
   insn->fixed = 1u;
   if (executionScope == spv::Scope::Subgroup)
      insn->subOp = NV50_IR_SUBOP_BAR_ARRIVE;
   else if (executionScope == spv::Scope::Workgroup)
      insn->subOp = NV50_IR_SUBOP_BAR_SYNC;

   info->numBarriers = 1u;

   return SPV_SUCCESS;
}

spv_result_t
Converter::generateMemBarrier(spv::Scope memoryScope, spv::MemorySemanticsMask memorySemantics)
{
   const spv::MemorySemanticsMask semantics = static_cast<spv::MemorySemanticsMask>(static_cast<uint32_t>(memorySemantics) & 0x0000001e);
   const spv::MemorySemanticsMask targets = static_cast<spv::MemorySemanticsMask>(static_cast<uint32_t>(memorySemantics)   & 0x00000fc0);

   if (hasFlag(targets, spv::MemorySemanticsShift::UniformMemory) ||
       hasFlag(targets, spv::MemorySemanticsShift::SubgroupMemory) ||
       hasFlag(targets, spv::MemorySemanticsShift::AtomicCounterMemory) ||
       hasFlag(targets, spv::MemorySemanticsShift::ImageMemory)) {
      _debug_printf("Only the workgroup memory semantics is currently supported.\n");
      return SPV_ERROR_INVALID_BINARY;
   }

   if (semantics != spv::MemorySemanticsMask::MaskNone)
      _debug_printf("MemBar semantics ignored: %02x\n", static_cast<uint32_t>(semantics));

   Instruction *insn = mkOp(OP_MEMBAR, TYPE_NONE, nullptr);
   insn->fixed = 1;
   switch (memoryScope) {
      case spv::Scope::Invocation:
      _debug_printf("Invocation scope is not supported for MemoryBarrier.\n");
      return SPV_UNSUPPORTED;
   case spv::Scope::Subgroup:
      _debug_printf("Subgroup scope is not supported for MemoryBarrier.\n");
      return SPV_UNSUPPORTED;
   case spv::Scope::Workgroup:
      insn->subOp = NV50_IR_SUBOP_MEMBAR(M, CTA);
      break;
   case spv::Scope::Device:
      insn->subOp = NV50_IR_SUBOP_MEMBAR(M, GL);
      break;
   case spv::Scope::CrossDevice:
      insn->subOp = NV50_IR_SUBOP_MEMBAR(M, SYS);
      break;
   }

   return SPV_SUCCESS;
}

spv_result_t
Converter::convertInstruction(const spv_parsed_instruction_t *parsedInstruction)
{
   auto getStruct = [&](spv::Id id){
      auto searchStruct = spvValues.find(id);
      return (searchStruct != spvValues.end()) ? searchStruct->second : SpirVValue{};
   };
   auto getIdOfOperand = [&](unsigned int operandIndex){
      const spv_parsed_operand_t parsedOperand = parsedInstruction->operands[operandIndex];
      return parsedInstruction->words[parsedOperand.offset];
   };
   auto getStructForOperand = [&](unsigned int operandIndex){
      return getStruct(getIdOfOperand(operandIndex));
   };
   auto getOp = [&](spv::Id id, unsigned c = 0u, bool constantsAllowed = true){
      auto searchOp = spvValues.find(id);
      if (searchOp == spvValues.end())
         return PValue();

      auto& opStruct = searchOp->second;
      if (c >= opStruct.value.size()) {
         _debug_printf("Trying to access element %u out of %u\n", c, opStruct.value.size());
         return PValue();
      }

      auto const pvalue = opStruct.value[c];
      auto op = pvalue.value;
      if (opStruct.storageFile == SpirvFile::IMMEDIATE) {
         if (!constantsAllowed)
            return PValue();
         auto constant = op;
         op = getScratch(constant->reg.size);
         mkMov(op, constant, constant->reg.type);
         return PValue(op);
      }
      return pvalue;
   };
   auto getType = [&](spv::Id id){
      auto searchType = types.find(id);
      if (searchType == types.end())
         return static_cast<const Type*>(nullptr);
      else
         return static_cast<const Type*>(searchType->second);
   };

   const spv::Op opcode = static_cast<spv::Op>(parsedInstruction->opcode);
   switch (opcode) {
   // TODO(pmoreau): Rework this
   case spv::Op::OpCapability:
      {
         using Cap = spv::Capability;
         Cap capability = spirv::getOperand<Cap>(parsedInstruction, 0u);
         if (info->target < 0xc0) {
            return SPV_SUCCESS;

            if (capability == Cap::Tessellation || capability == Cap::Vector16 ||
                capability == Cap::Float16Buffer || capability == Cap::Float16 ||
                capability == Cap::Float64 || capability == Cap::Int64 ||
                capability == Cap::Int64Atomics || capability == Cap::Int16 ||
                capability == Cap::TessellationPointSize || capability == Cap::Int8 ||
                capability == Cap::TransformFeedback) {
               _debug_printf("Capability unsupported: %u\n", capability);
               return SPV_UNSUPPORTED;
            }
         }
      }
      break;
   case spv::Op::OpExtInstImport:
      {
         const char *setName = spirv::getOperand<const char*>(parsedInstruction, 1u);
         if (std::strcmp(setName, "OpenCL.std")) {
            pipe_debug_message(info->debug, ERROR,
                               "Unsupported extended instruction set \"%s\"\n",
                               setName);
            return SPV_UNSUPPORTED;
         }
      }
      break;
   case spv::Op::OpExtInst:
      {
         const spv::Id id = parsedInstruction->result_id;
         const Type *type = types.find(parsedInstruction->type_id)->second;
         const word extensionOpcode = spirv::getOperand<word>(parsedInstruction, 3u);

         switch (parsedInstruction->ext_inst_type) {
         case SPV_EXT_INST_TYPE_OPENCL_STD:
            return convertOpenCLInstruction(id, type, static_cast<OpenCLLIB::Entrypoints>(extensionOpcode), parsedInstruction);
         default:
            pipe_debug_message(info->debug, ERROR,
                               "Unsupported SPV_EXT_INST_TYPE %u\n",
                               parsedInstruction->ext_inst_type);
            return SPV_UNSUPPORTED;
         }
      }
      break;
   case spv::Op::OpMemoryModel:
      addressingModel = spirv::getOperand<spv::AddressingModel>(parsedInstruction, 0u);
      memoryModel = spirv::getOperand<spv::MemoryModel>(parsedInstruction, 1u);
      break;
   case spv::Op::OpEntryPoint:
      return convertEntryPoint(parsedInstruction);
   // TODO(pmoreau): Properly handle the different execution modes
   case spv::Op::OpExecutionMode:
      {
         const spv::Id entryPointId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         const spv::ExecutionMode executionMode = spirv::getOperand<spv::ExecutionMode>(parsedInstruction, 1u);
         pipe_debug_message(info->debug, INFO,
                            "Ignoring unsupported execution mode %u for entry point %u\n",
                            executionMode, entryPointId);
      }
      break;
   case spv::Op::OpName:
      names.emplace(getIdOfOperand(0u), spirv::getOperand<const char*>(parsedInstruction, 1u));
   case spv::Op::OpSourceContinued:
   case spv::Op::OpSource:
   case spv::Op::OpSourceExtension:
   case spv::Op::OpMemberName:
   case spv::Op::OpString:
   case spv::Op::OpLine:
   case spv::Op::OpNoLine:
      break;
   case spv::Op::OpDecorate:
      return convertDecorate(parsedInstruction);
   case spv::Op::OpMemberDecorate:
      pipe_debug_message(info->debug, ERROR,
                         "OpMemberDecorate is unsupported.\n");
      return SPV_UNSUPPORTED;
   case spv::Op::OpDecorationGroup:
      break;
   case spv::Op::OpGroupDecorate:
      {
         const Decoration &groupDecorations = decorations.find(getIdOfOperand(0u))->second;

         for (unsigned int i = 1u; i < parsedInstruction->num_operands; ++i) {
            Decoration &targetDecorations = decorations[getIdOfOperand(i)];
            for (const auto &decoration : groupDecorations)
               targetDecorations[decoration.first].insert(targetDecorations[decoration.first].end(), decoration.second.begin(), decoration.second.end());
         }
      }
      break;
   case spv::Op::OpTypeVoid:
      return convertType<TypeVoid>(parsedInstruction);
   case spv::Op::OpTypeBool:
      return convertType<TypeBool>(parsedInstruction);
   case spv::Op::OpTypeInt:
      return convertType<TypeInt>(parsedInstruction);
   case spv::Op::OpTypeFloat:
      return convertType<TypeFloat>(parsedInstruction);
   case spv::Op::OpTypeStruct:
      return convertType<TypeStruct>(parsedInstruction);
   case spv::Op::OpTypeVector:
      return convertType<TypeVector>(parsedInstruction);
   case spv::Op::OpTypeArray:
      return convertType<TypeArray>(parsedInstruction);
   case spv::Op::OpTypePointer:
      return convertType<TypePointer>(parsedInstruction);
   case spv::Op::OpTypeFunction:
      return convertType<TypeFunction>(parsedInstruction);
   case spv::Op::OpTypeSampler:
      return convertType<TypeSampler>(parsedInstruction);
   case spv::Op::OpTypeImage:
      return convertType<TypeImage>(parsedInstruction);
   case spv::Op::OpTypeSampledImage:
      return convertType<TypeSampledImage>(parsedInstruction);
   case spv::Op::OpTypeEvent:
      return convertType<TypeEvent>(parsedInstruction);
   case spv::Op::OpConstant:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         uint16_t operandIndex = 2u;
         const auto constants = resType->generateConstant(*this, parsedInstruction, operandIndex);
         std::vector<PValue> values;
         values.reserve(constants.size());
         for (ImmediateValue *c : constants)
            values.emplace_back(c);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::IMMEDIATE, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpConstantTrue:
   case spv::Op::OpConstantFalse:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const auto constants = resType->generateBoolConstant(*this, opcode == spv::Op::OpConstantTrue);
         std::vector<PValue> values;
         values.reserve(constants.size());
         for (ImmediateValue *c : constants)
            values.emplace_back(c);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::IMMEDIATE, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpConstantNull:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const auto constants = resType->generateNullConstant(*this);
         std::vector<PValue> values;
         values.reserve(constants.size());
         for (Value *c : constants)
            values.emplace_back(c);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::IMMEDIATE, resType, values, resType->getPaddings() });
      }
   case spv::Op::OpConstantComposite:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         std::vector<PValue> values;
         values.reserve(parsedInstruction->num_operands - 2u);
         for (unsigned int i = 2u; i < parsedInstruction->num_operands; ++i) {
            const SpirVValue &op = getStructForOperand(i);
            values.insert(values.end(), op.value.cbegin(), op.value.cend());
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::IMMEDIATE, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpConstantSampler:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const auto addressingMode = spirv::getOperand<spv::SamplerAddressingMode>(parsedInstruction, 2u);
         const word param = spirv::getOperand<word>(parsedInstruction, 3u);
         const auto filterMode = spirv::getOperand<spv::SamplerFilterMode>(parsedInstruction, 4u);
         const bool usesNormalizedCoords = param == 0u;

         samplers.emplace(resId, Sampler{ reinterpret_cast<TypeSampler const*>(resType), 0u, addressingMode, usesNormalizedCoords, filterMode });
      }
      break;
   case spv::Op::OpVariable:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         auto storageFile = getStorageFile(spirv::getOperand<spv::StorageClass>(parsedInstruction, 2u));

         bool isBuiltIn = false;
         auto searchDecorations = decorations.find(resId);
         if (searchDecorations != decorations.end()) {
            isBuiltIn = searchDecorations->second.find(spv::Decoration::BuiltIn) != searchDecorations->second.end();
            auto searchLinkage = searchDecorations->second.find(spv::Decoration::BuiltIn);
            if (!isBuiltIn && searchLinkage != searchDecorations->second.end() && static_cast<spv::LinkageType>(searchLinkage->second[0][0]) == spv::LinkageType::Import) {
               _debug_printf("Variable %u has linkage type \"import\"! Missing a link step?\n", resId);
               return SPV_ERROR_INVALID_POINTER;
            }
         }

         if (parsedInstruction->num_operands == 4u) {
            const SpirVValue &init = getStructForOperand(3u);

            // If we have an immediate, which is stored in const memory,
            // inline it
            if (storageFile == SpirvFile::CONST && init.storageFile == SpirvFile::IMMEDIATE)
               storageFile = SpirvFile::IMMEDIATE;
            spvValues.emplace(resId, SpirVValue{ storageFile, resType, init.value, init.paddings });
         } else if (resType->type == spv::Op::OpTypePointer &&
                    reinterpret_cast<const TypePointer*>(resType)->getPointedType()->type == spv::Op::OpTypeEvent) {
            assert(storageFile == SpirvFile::TEMPORARY);
            spvValues.emplace(resId, SpirVValue{ SpirvFile::NONE, resType, { nullptr }, { } });
         } else if (!isBuiltIn) {
            acquire(storageFile, resId, resType);
         }
      }
      break;
   case spv::Op::OpNop:
      break;
   case spv::Op::OpUndef:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         auto constants = resType->generateNullConstant(*this);
         std::vector<PValue> res;
         for (auto i : constants)
            res.push_back(i);
         spvValues.emplace(resId, SpirVValue{ SpirvFile::IMMEDIATE, resType, res, resType->getPaddings() });
      }
      break;
   // TODO:
   // * use FunctionControl
   // * use decorations
   case spv::Op::OpFunction:
      {
         spv::Id id = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         auto searchFunc = functions.find(id);
         if (searchFunc != functions.end()) {
            func = searchFunc->second;
            setPosition(BasicBlock::get(func->cfg.getRoot()), true);
            return SPV_SUCCESS;
         }

         const Type *resType = types.find(parsedInstruction->type_id)->second;

         using FCM = spv::FunctionControlMask;
         FCM control = spirv::getOperand<FCM>(parsedInstruction, 2u);

         auto searchName = names.find(id);
         if (searchName == names.end()) {
            _debug_printf("Couldn't find a name for function\n");
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto searchEntry = entryPoints.find(id);
         auto const label = (searchEntry == entryPoints.end()) ? UINT32_MAX : searchEntry->second.index; // XXX valid symbol needed for functions?
         auto &name = searchName->second;
         char *funcName = new char[name.size() + 1u];
         std::strncpy(funcName, name.c_str(), name.size() + 1u);
         Function *function = new Function(prog, funcName, label);
         functions.emplace(id, function);
         func = function;
         currentFuncId = id;
         samplerCounter = 0u;
         imageCounter = 0u;

         prog->main->call.attach(&func->call, Graph::Edge::TREE); // XXX only bind entry points to main?
         BasicBlock *block = new BasicBlock(func);
         func->setEntry(block);
         func->setExit(new BasicBlock(func));
         prog->calls.insert(&func->call);

         if (!resType->isVoidType())
            func->outs.emplace_back(getScratch(resType->getSize()));

         setPosition(block, true);
      }
      break;
   // TODO:
   // * use decorations
   case spv::Op::OpFunctionParameter:
      {
         spv::Id id = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         spv::Id type = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto search = types.find(type);
         if (search == types.end()) {
            _debug_printf("Couldn't find type associated to id %u\n", type);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         bool isKernel = false;
         auto searchEntry = entryPoints.find(currentFuncId);
         if (searchEntry != entryPoints.end())
            isKernel = (searchEntry->second.executionModel == spv::ExecutionModel::Kernel);

         Type *paramType = search->second;

         // Samplers are not passed in the user input buffer.
         if (paramType->getType() == spv::Op::OpTypeSampler) {
            assert(isKernel);
            samplers.emplace(id, Sampler{ reinterpret_cast<const TypeSampler*>(paramType), samplerCounter++ });
            return SPV_SUCCESS;
         }

         if (paramType->getType() == spv::Op::OpTypeImage) {
            assert(isKernel);
            images.emplace(id, Image{ reinterpret_cast<const TypeImage*>(paramType), imageCounter++ });
         }

         SpirvFile destStorageFile = (paramType->getType() != spv::Op::OpTypePointer) ? SpirvFile::TEMPORARY : reinterpret_cast<const TypePointer *>(paramType)->getStorageFile();
         auto decos = decorations.find(id);
         if (decos != decorations.end()) {
            auto paramAttrs = decos->second.find(spv::Decoration::FuncParamAttr);
            if (paramAttrs != decos->second.end() && static_cast<spv::FunctionParameterAttribute>(paramAttrs->second[0][0]) == spv::FunctionParameterAttribute::ByVal) {
               paramType = reinterpret_cast<const TypePointer *>(search->second)->getPointedType();
               destStorageFile = SpirvFile::TEMPORARY;
            }
         }
         if (isKernel) {
            inputOffset += load(destStorageFile, SpirvFile::INPUT, id, { PValue() }, inputOffset, paramType);
            spvValues[id].type = search->second;
         } else {
            std::vector<PValue> values;
            std::stack<Type const*> stack;
            stack.push(paramType);
            while (!stack.empty()) {
               unsigned deltaOffset = 0u;
               auto currentType = stack.top();
               stack.pop();
               if (!currentType->isCompooundType()) {
                  Value *res = getScratch(std::max(4u, currentType->getSize()));
                  values.emplace_back(nullptr, res);
                  func->ins.emplace_back(res);
               } else {
                  for (unsigned int i = currentType->getElementsNb(); i != 0u; --i)
                     stack.push(currentType->getElementType(i - 1u));
               }
            }
            spvValues.emplace(id, SpirVValue{ SpirvFile::TEMPORARY, paramType, values, paramType->getPaddings() });
         }
      }
      break;
   case spv::Op::OpFunctionEnd:
      {
         for (auto i : phiToMatch) {
            auto phiId = i.first;
            auto searchPhiData = phiNodes.find(phiId);
            if (searchPhiData == phiNodes.end()) {
               _debug_printf("Couldn't find phi data for id %u\n", phiId);
               return SPV_ERROR_INVALID_LOOKUP;
            }
            for (auto j : i.second) {
               auto index = j.first;
               auto varId = j.second.first;
               auto& varBbPair = searchPhiData->second[index];
               if (varId != 0u) {
                  auto searchVar = spvValues.find(varId);
                  if (searchVar == spvValues.end()) {
                     _debug_printf("Couldn't find variable with id %u\n", varId);
                     return SPV_ERROR_INVALID_LOOKUP;
                  }
                  varBbPair.first = searchVar->second.value;
                  _debug_printf("Found var with id %u: %p\n", varId, varBbPair.first[0]);
               }
               auto bbId = j.second.second;
               if (bbId != 0u) {
                  auto searchBb = blocks.find(bbId);
                  if (searchBb == blocks.end()) {
                     _debug_printf("Couldn't find BB with id %u\n", bbId);
                     return SPV_ERROR_INVALID_LOOKUP;
                  }
                  varBbPair.second = searchBb->second;
                  _debug_printf("Found bb with id %u: %p\n", bbId, varBbPair.second);
               }
            }
         }
         phiToMatch.clear();

         // Debugging purposes
         {
            for (auto block : blocks) {
               auto lBb = block.second;
               Instruction *next;
               for (Instruction *i = lBb->getPhi(); i && i != lBb->getEntry(); i = next) {
                  next = i->next;
                  auto searchPhi = phiMapping.find(i);
                  if (searchPhi == phiMapping.end()) {
                     assert(false);
                     return SPV_ERROR_INTERNAL;
                  }
                  auto searchPhiData = phiNodes.find(searchPhi->second);
                  if (searchPhiData == phiNodes.end()) {
                     assert(false);
                     return SPV_ERROR_INTERNAL;
                  }
                  auto counter = 0u;
                  for (auto& phiPair : searchPhiData->second) {
                     i->setSrc(0, phiPair.first[0].value);
                     ++counter;
                  }
               }
            }
         }

         if (!branchesToMatch.empty()) {
            _debug_printf("Could not match some branches!\n");
            for (auto const& i : branchesToMatch) {
               _debug_printf("\t%u: ", i.first);
               for (auto const& j : i.second)
                  _debug_printf("%p ", j);
               _debug_printf("\n");
            }
         }

         BasicBlock *leave = BasicBlock::get(func->cfgExit);
         setPosition(leave, true);
         if (entryPoints.find(static_cast<spv::Id>(func->getLabel())) != entryPoints.end())
            mkFlow(OP_EXIT, nullptr, CC_ALWAYS, nullptr)->fixed = 1;
         else
            mkFlow(OP_RET, nullptr, CC_ALWAYS, nullptr)->fixed = 1;

         blocks.clear();
         func = nullptr;
         currentFuncId = 0u;
         inputOffset = 0u;
         samplerCounter = 0u;
         imageCounter = 0u;
      }
      break;
   case spv::Op::OpFunctionCall:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const spv::Id functionId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);

         FlowInstruction *insn = mkFlow(OP_CALL, nullptr, CC_ALWAYS, NULL);

         for (size_t i = 3u; i < parsedInstruction->num_operands; ++i)
            insn->setSrc(i - 3u, getStructForOperand(i).value.front().value);

         if (!resType->isVoidType()) {
            Value *res = getScratch(resType->getSize());
            insn->setDef(0, res);
            spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, { res }, resType->getPaddings() });
         }

         auto functionToMatchIter = functionsToMatch.find(functionId);
         if (functionToMatchIter == functionsToMatch.end())
            functionsToMatch.emplace(functionId, std::vector<FunctionData>{ FunctionData{ func, insn } });
         else
            functionToMatchIter->second.emplace_back(func, insn);
      }
      break;
   case spv::Op::OpLabel:
      {
         // A BB is created on function creation to store loads of arguments,
         // so only create one if this is not the first BB of the function.
         if (!blocks.empty())
            setPosition(new BasicBlock(func), true);

         const spv::Id id = parsedInstruction->result_id;
         blocks.emplace(id, bb);

         auto searchFlows = branchesToMatch.find(id);
         if (searchFlows != branchesToMatch.end()) {
            for (auto& flow : searchFlows->second) {
               flow->bb->getExit()->asFlow()->target.bb = bb;
               flow->bb->cfg.attach(&bb->cfg, (bb->cfg.incidentCount() == 0u) ? Graph::Edge::TREE : Graph::Edge::FORWARD);
            }
            branchesToMatch.erase(searchFlows);
         }
      }
      break;
   case spv::Op::OpReturn:
      {
         BasicBlock *leave = BasicBlock::get(func->cfgExit);
         mkFlow(OP_BRA, leave, CC_ALWAYS, nullptr);
         bb->cfg.attach(&leave->cfg, (leave->cfg.incidentCount() == 0) ? Graph::Edge::TREE : Graph::Edge::FORWARD);
      }
      break;
   case spv::Op::OpReturnValue:
      {
         auto retId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto retIter = spvValues.find(retId);
         if (retIter == spvValues.end()) {
            _debug_printf("Couldn't find value %u returned\n", retId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         assert(func->outs.size() == 1u);
         mkOp1(OP_MOV, retIter->second.type->getEnumType(), func->outs.front().get(), retIter->second.value.front().value);

         BasicBlock *leave = BasicBlock::get(func->cfgExit);
         mkFlow(OP_BRA, leave, CC_ALWAYS, nullptr);
         bb->cfg.attach(&leave->cfg, (leave->cfg.incidentCount() == 0) ? Graph::Edge::TREE : Graph::Edge::FORWARD);
      }
      break;
   case spv::Op::OpBranch:
      {
         auto labelId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto searchLabel = blocks.find(labelId);
         if (searchLabel == blocks.end()) {
            auto flow = mkFlow(OP_BRA, nullptr, CC_ALWAYS, nullptr);
            auto searchFlow = branchesToMatch.find(labelId);
            if (searchFlow == branchesToMatch.end())
               branchesToMatch.emplace(labelId, std::vector<FlowInstruction*>{ flow });
            else
               searchFlow->second.push_back(flow);
         } else {
            mkFlow(OP_BRA, searchLabel->second, CC_ALWAYS, nullptr);
            bb->cfg.attach(&searchLabel->second->cfg, Graph::Edge::BACK);
            // XXX We have a loop?
            func->loopNestingBound++;
         }
      }
      break;
   case spv::Op::OpBranchConditional:
      {
         Value *pred = getStructForOperand(0u).value.front().value;
         const spv::Id ifId = getIdOfOperand(1u);
         const spv::Id elseId = getIdOfOperand(2u);

         auto searchIf = blocks.find(ifId);
         if (searchIf == blocks.end()) {
            auto flow = mkFlow(OP_BRA, nullptr, CC_P, pred);
            auto searchFlow = branchesToMatch.find(ifId);
            if (searchFlow == branchesToMatch.end())
               branchesToMatch.emplace(ifId, std::vector<FlowInstruction*>{ flow });
            else
               searchFlow->second.push_back(flow);
         } else {
            mkFlow(OP_BRA, searchIf->second, CC_P, pred);
            bb->cfg.attach(&searchIf->second->cfg, Graph::Edge::BACK);
         }

         auto tmp = new BasicBlock(func);
         bb->cfg.attach(&tmp->cfg, Graph::Edge::TREE);
         setPosition(tmp, true);

         auto searchElse = blocks.find(elseId);
         if (searchElse == blocks.end()) {
            auto flow = mkFlow(OP_BRA, nullptr, CC_ALWAYS, nullptr);
            auto searchFlow = branchesToMatch.find(elseId);
            if (searchFlow == branchesToMatch.end())
               branchesToMatch.emplace(elseId, std::vector<FlowInstruction*>{ flow });
            else
               searchFlow->second.push_back(flow);
         } else {
            mkFlow(OP_BRA, searchElse->second, CC_ALWAYS, nullptr);
            bb->cfg.attach(&searchElse->second->cfg, Graph::Edge::BACK);
         }
      }
      break;
   case spv::Op::OpPhi:
      {
         auto typeId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto resId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         auto searchType = types.find(typeId);
         if (searchType == types.end()) {
            _debug_printf("Couldn't find type with id %u\n", typeId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto type = searchType->second;
         auto parents = std::vector<std::pair<std::vector<PValue>, BasicBlock*>>();
         auto toMatchs = std::unordered_map<uint32_t, std::pair<spv::Id, spv::Id>>();
         for (unsigned int i = 2u, counter = 0u; i < parsedInstruction->num_operands; i += 2u, ++counter) {
            auto vars = std::vector<PValue>();
            auto varId = spirv::getOperand<spv::Id>(parsedInstruction, i);
            auto toMatch = std::make_pair<spv::Id, spv::Id>(0u, 0u);
            for (unsigned int j = 0u; j < type->getElementsNb(); ++j) {
               auto var = getOp(varId, j, false).value;
               if (var == nullptr) {
                  _debug_printf("Couldn't find variable with id %u, keeping looking for it\n", varId);
                  toMatch.first = varId;
               }
               vars.push_back(var);
            }
            auto bbId = spirv::getOperand<spv::Id>(parsedInstruction, i + 1);
            auto searchBB = blocks.find(bbId);
            if (searchBB == blocks.end()) {
               _debug_printf("Couldn't find BB with id %u, keeping looking for it\n", bbId);
               toMatch.second = bbId;
            }
            if (toMatch.first != 0u || toMatch.second != 0u)
               toMatchs.emplace(counter, toMatch);
            parents.emplace_back(vars, (searchBB != blocks.end()) ? searchBB->second : nullptr);
         }
         auto value = std::vector<PValue>();
         if (type->getElementsNb() > 1u)
            _debug_printf("OpPhi on type with more than 1 element: need to check behaviour!\n");
         for (unsigned int i = 0u; i < type->getElementsNb(); ++i)
            value.push_back(getScratch(type->getElementSize(i)));
         auto phi = new_Instruction(func, OP_PHI, nv50_ir::TYPE_U32); // This instruction will be removed later, so we don't care about the size
         phi->setDef(0, value[0].value);
         bb->insertTail(phi);
         phiNodes.emplace(resId, parents);
         phiMapping.emplace(phi, resId);
         if (!toMatchs.empty())
            phiToMatch.emplace(resId, toMatchs);
         spvValues.emplace(resId, SpirVValue{ SpirvFile::NONE, type, value, type->getPaddings() });
      }
      break;
   case spv::Op::OpSwitch:
      {
         spv::Id selectorId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto searchSelector = spvValues.find(selectorId);
         if (searchSelector == spvValues.end()) {
            _debug_printf("Could not find selector with id %u\n", selectorId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto type = searchSelector->second.type;
         BasicBlock *newBb = bb;
         BasicBlock *oldBb = bb;
         for (size_t i = 2u; i < parsedInstruction->num_operands; i += 2u) {
            uint16_t operandIndex = i;
            auto imm = type->generateConstant(*this, parsedInstruction, operandIndex).front();
            auto imm2 = getScratch(type->getSize());
            mkMov(imm2, imm, type->getEnumType());
            auto const labelId = spirv::getOperand<spv::Id>(parsedInstruction, i + 1u);
            auto pred = getScratch(1, FILE_PREDICATE);
            mkCmp(OP_SET, CC_EQ, TYPE_U32, pred, type->getEnumType(), searchSelector->second.value[0].value, imm2);
            auto searchLabel = blocks.find(labelId);
            if (searchLabel == blocks.end()) {
               auto flow = mkFlow(OP_BRA, nullptr, CC_P, pred);
               auto searchFlow = branchesToMatch.find(labelId);
               if (searchFlow == branchesToMatch.end())
                  branchesToMatch.emplace(labelId, std::vector<FlowInstruction*>{ flow });
               else
                  searchFlow->second.push_back(flow);
            } else {
               mkFlow(OP_BRA, searchLabel->second, CC_P, pred);
               oldBb->cfg.attach(&searchLabel->second->cfg, Graph::Edge::BACK);
            }
            newBb = new BasicBlock(func);
            oldBb->cfg.attach(&newBb->cfg, Graph::Edge::TREE);
            setPosition(newBb, true);
            oldBb = newBb;
         }

         auto const defaultId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         auto searchLabel = blocks.find(defaultId);
         if (searchLabel == blocks.end()) {
            auto flow = mkFlow(OP_BRA, nullptr, CC_ALWAYS, nullptr);
            auto searchFlow = branchesToMatch.find(defaultId);
            if (searchFlow == branchesToMatch.end())
               branchesToMatch.emplace(defaultId, std::vector<FlowInstruction*>{ flow });
            else
               searchFlow->second.push_back(flow);
         } else {
            mkFlow(OP_BRA, searchLabel->second, CC_ALWAYS, nullptr);
            newBb->cfg.attach(&searchLabel->second->cfg, Graph::Edge::BACK);
         }

         bb = nullptr;
      }
      break;
   case spv::Op::OpLifetimeStart: // FALLTHROUGH
   case spv::Op::OpLifetimeStop:
      break;
   case spv::Op::OpLoad:
      {
         auto typeId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto searchType = types.find(typeId);
         if (searchType == types.end()) {
            _debug_printf("Couldn't find type with id %u\n", typeId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto type = searchType->second;
         spv::Id resId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         spv::Id pointerId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);
         spv::MemoryAccessMask access = spv::MemoryAccessMask::MaskNone;
         if (parsedInstruction->num_operands == 4u)
            access = spirv::getOperand<spv::MemoryAccessMask>(parsedInstruction, 3u);
         uint32_t alignment = 0u;
         if (hasFlag(access, spv::MemoryAccessShift::Aligned))
            alignment = spirv::getOperand<unsigned>(parsedInstruction, 4u);

         auto searchDecorations = decorations.find(pointerId);
         if (searchDecorations != decorations.end()) {
            for (auto& decoration : searchDecorations->second) {
               if (decoration.first != spv::Decoration::BuiltIn)
                  continue;

               return loadBuiltin(resId, type, decoration.second.front(), access);
            }
         }

         auto searchPointer = spvValues.find(pointerId);
         if (searchPointer == spvValues.end()) {
            _debug_printf("Couldn't find pointer with id %u\n", pointerId);
            return SPV_ERROR_INVALID_LOOKUP;
         }

         auto pointerType = reinterpret_cast<const TypePointer *>(searchPointer->second.type);
         load(SpirvFile::TEMPORARY, pointerType->getStorageFile(), resId, searchPointer->second.value, 0u, type, access, alignment);
      }
      break;
   case spv::Op::OpStore:
      {
         spv::Id pointerId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         spv::Id objectId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         spv::MemoryAccessMask access = spv::MemoryAccessMask::MaskNone;
         if (parsedInstruction->num_operands == 3u)
            access = spirv::getOperand<spv::MemoryAccessMask>(parsedInstruction, 2u);
         uint32_t alignment = 0u;
         if (hasFlag(access, spv::MemoryAccessShift::Aligned))
            alignment = spirv::getOperand<unsigned>(parsedInstruction, 3u);

         auto searchPointer = spvValues.find(pointerId);
         if (searchPointer == spvValues.end()) {
            _debug_printf("Couldn't find pointer with id %u\n", pointerId);
            return SPV_ERROR_INVALID_LOOKUP;
         }

         auto pointerType = reinterpret_cast<const TypePointer *>(searchPointer->second.type);
         auto searchElementStruct = spvValues.find(objectId);
         if (searchElementStruct == spvValues.end()) {
            _debug_printf("Couldn't find object with id %u\n", objectId);
            return SPV_ERROR_INVALID_LOOKUP;
         }

         // TODO(pmoreau): this is a hack as TypeEvent is not a physical value
         if (pointerType->getPointedType()->type == spv::Op::OpTypeEvent)
            return SPV_SUCCESS;

         auto value = searchElementStruct->second.value;
         store(pointerType->getStorageFile(), searchPointer->second.value, 0u, value, pointerType->getPointedType(), access, alignment);
      }
      break;
   // TODO(pmoreau): Should have another look at it.
   // * AccessChain -> struct, array, vector, etc.
   // * PtrAccessChain -> dereference a pointer array, i.e. float* = {3,4,5}
   case spv::Op::OpPtrAccessChain: // FALLTHROUGH
   case spv::Op::OpInBoundsPtrAccessChain:
      {
         auto resTypeId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto resId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         auto baseId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);

         auto searchBaseStruct = spvValues.find(baseId);
         if (searchBaseStruct == spvValues.end()) {
            _debug_printf("Couldn't find base with id %u\n", baseId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto& baseStruct = searchBaseStruct->second;
         auto base = baseStruct.value[0];
         auto baseType = baseStruct.type;

         std::vector<Value *> indices;
         for (unsigned int i = 3u; i < parsedInstruction->num_operands; ++i) {
            auto elementId = spirv::getOperand<spv::Id>(parsedInstruction, i);
            auto searchElementStruct = spvValues.find(elementId);
            if (searchElementStruct == spvValues.end()) {
               _debug_printf("Couldn't find element with id %u\n", elementId);
               return SPV_ERROR_INVALID_LOOKUP;
            }

            auto& elementStruct = searchElementStruct->second;
            auto element = elementStruct.value[0];
            indices.push_back(element.value);
         }

         auto resTypeIter = types.find(resTypeId);
         if (resTypeIter == types.end()) {
            _debug_printf("Couldn't find pointer type of id %u\n", resTypeId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto resType = reinterpret_cast<const TypePointer *>(resTypeIter->second);

         // If in GPRs, copy from indexed element up to the last element
         // Otherwise, compute offset as before

         std::vector<PValue> values;
         std::vector<unsigned int> paddings;
         if (baseStruct.storageFile == SpirvFile::TEMPORARY || baseStruct.storageFile == SpirvFile::IMMEDIATE) {
            unsigned index = 0u, depth = 0u;
            std::stack<const Type *> typesStack;
            typesStack.push(baseStruct.type);
            while (!typesStack.empty()) {
               const Type *currentType = typesStack.top();
               typesStack.pop();
               assert(currentType->isCompooundType() || currentType->getType() == spv::Op::OpTypePointer);
               if (currentType->getType() == spv::Op::OpTypePointer) {
               } else {
                  std::function<unsigned int (const Type *type)> const &getMembersCount = [&getMembersCount](const Type *type) {
                     if (!type->isCompooundType())
                        return 1u;
                     unsigned membersCount = 0u;
                     for (unsigned int i = 0u; i < type->getElementsNb(); ++i)
                        membersCount += getMembersCount(type->getElementType(i));
                     return membersCount;
                  };
                  int memberIndex = indices[depth]->reg.data.s32;
                  for (int i = memberIndex - 1; i >= 0; --i)
                     index += getMembersCount(currentType->getElementType(i));
                  typesStack.push(currentType->getElementType(memberIndex));
                  ++depth;
               }
            }
            values.insert(values.end(), baseStruct.value.begin() + index, baseStruct.value.end());
            paddings.insert(paddings.end(), baseStruct.paddings.begin() + index, baseStruct.paddings.end());
         } else {
            auto offset = getScratch((info->target < 0xc0) ? 4u : 8u);
            if (info->target < 0xc0) {
               offset->reg.type = TYPE_U32;
               loadImm(offset, 0u);
            } else {
               offset->reg.type = TYPE_U64;
               loadImm(offset, 0lu);
            }
            auto searchDecorations = decorations.find(baseId);
            baseType->getGlobalOffset(this, (searchDecorations != decorations.end()) ? searchDecorations->second : Decoration(), offset, indices);
            if (base.isValue()) {
               auto ptr = getScratch(offset->reg.size);
               mkOp2(OP_ADD, offset->reg.type, ptr, base.value, offset);
               values.emplace_back(nullptr, ptr);
            } else {
               values.emplace_back(base.symbol, offset);
            }
            paddings.push_back(1u);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, paddings });
      }
      break;
   case spv::Op::OpCompositeExtract:
      {
         auto typeId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         auto resId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         auto baseId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);

         auto searchBaseStruct = spvValues.find(baseId);
         if (searchBaseStruct == spvValues.end()) {
            _debug_printf("Couldn't find base with id %u\n", baseId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto& baseStruct = searchBaseStruct->second;
         auto base = baseStruct.value[0];

         auto type = types.find(typeId);
         if (type == types.end()) {
            _debug_printf("Couldn't find type with id %u\n", typeId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto baseType = baseStruct.type;

         auto ids = std::vector<unsigned int>();
         for (unsigned int i = 3u; i < parsedInstruction->num_operands; ++i)
            ids.push_back(spirv::getOperand<unsigned int>(parsedInstruction, i));
         auto offset = baseType->getGlobalIdx(ids);

         Value* dst = nullptr;
         if (base.isValue()) {
            auto searchSrc = spvValues.find(baseId);
            if (searchSrc == spvValues.end()) {
               _debug_printf("Member %u of id %u couldn't be found\n", offset, baseId);
               return SPV_ERROR_INVALID_LOOKUP;
            }
            auto& value = searchSrc->second.value;
            if (offset >= value.size()) {
               _debug_printf("Trying to access member %u out of %u\n", offset, value.size());
               return SPV_ERROR_INVALID_LOOKUP;
            }
            auto src = value[offset].value;
            dst = getScratch(std::max(4u, type->second->getSize()));
            mkMov(dst, src, typeOfSize(std::max(4u, typeSizeof(type->second->getEnumType()))));
            spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type->second, { dst }, type->second->getPaddings() });
         } else {
            load(SpirvFile::TEMPORARY, baseStruct.storageFile, baseId, { PValue() }, offset, baseType);
         }
      }
      break;
   case spv::Op::OpCompositeInsert:
      {
         const spv::Id typeId = parsedInstruction->type_id;
         const spv::Id resId = parsedInstruction->result_id;
         const spv::Id objId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);
         const spv::Id baseId = spirv::getOperand<spv::Id>(parsedInstruction, 3u);
         std::vector<unsigned int> ids;
         for (uint16_t i = 4u; i < parsedInstruction->num_operands; ++i)
            ids.push_back(spirv::getOperand<unsigned int>(parsedInstruction, i));

         const PValue obj = getOp(objId);
         assert(!obj.isUndefined());

         const SpirVValue &baseStruct = getStruct(baseId);
         assert(!baseStruct.isUndefined());
         const std::vector<PValue> &baseValues = baseStruct.value;
         const Type *baseType = baseStruct.type;

         const Type *returnType = getType(typeId);
         assert(returnType != nullptr);
         const unsigned int offset = baseType->getGlobalIdx(ids);
         assert(offset < baseValues.size());

         std::vector<PValue> res(baseValues.size());
         for (unsigned int i = 0u; i < baseValues.size(); ++i) {
            const unsigned int resultSize = std::max(4u, returnType->getElementSize(i));
            Value *src = (i != offset) ? baseValues[i].value : obj.value;
            res[i] = getScratch(resultSize);
            mkMov(res[i].value, src, typeOfSize(resultSize));
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, returnType, res, returnType->getPaddings() });
      }
      break;
   case spv::Op::OpBitcast:
      {
         const auto resTypeId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         const auto resId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         const auto operandId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);

         auto type = types.find(resTypeId);
         if (type == types.end()) {
            _debug_printf("Couldn't find type with id %u\n", resTypeId);
            return SPV_ERROR_INVALID_LOOKUP;
         }

         auto op = spvValues.find(operandId);
         if (op == spvValues.end()) {
            _debug_printf("Couldn't find op with id %u\n", operandId);
            return SPV_ERROR_INVALID_LOOKUP;
         }

         auto storageFile = SpirvFile::TEMPORARY;
         const Type *resType = type->second;
         if (type->second->getType() == spv::Op::OpTypePointer) {
            storageFile = reinterpret_cast<TypePointer const*>(type->second)->getStorageFile();
            // If we have an immediate, which are stored in const memory,
            // inline it
            if (storageFile == SpirvFile::CONST && op->second.storageFile == SpirvFile::IMMEDIATE)
               storageFile = SpirvFile::IMMEDIATE;

            // If we bitcast a pointer to regs to another pointer to regs, keep
            // the old type which has more info.
            if (op->second.type->getType() == spv::Op::OpTypePointer && op->second.storageFile == SpirvFile::TEMPORARY)
               resType = op->second.type;
         }

         spvValues.emplace(resId, SpirVValue{ storageFile, resType, op->second.value, op->second.paddings });
      }
      break;
   case spv::Op::OpCopyMemory: // FALLTHROUGH
   case spv::Op::OpCopyMemorySized:
      {
         const auto targetId = spirv::getOperand<spv::Id>(parsedInstruction, 0u);
         const auto sourceId = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
         const auto access = ((opcode == spv::Op::OpCopyMemory && parsedInstruction->num_operands > 2u) || (opcode == spv::Op::OpCopyMemorySized && parsedInstruction->num_operands > 3u)) ? spirv::getOperand<spv::MemoryAccessMask>(parsedInstruction, 2u + (opcode == spv::Op::OpCopyMemorySized)) : spv::MemoryAccessMask::MaskNone;
         const auto alignment = hasFlag(access, spv::MemoryAccessShift::Aligned) ? spirv::getOperand<uint32_t>(parsedInstruction, 4u) : 1u;

         auto target = spvValues.find(targetId);
         if (target == spvValues.end()) {
            _debug_printf("Couldn't find target with id %u\n", targetId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         auto source = spvValues.find(sourceId);
         if (source == spvValues.end()) {
            _debug_printf("Couldn't find source with id %u\n", sourceId);
            return SPV_ERROR_INVALID_LOOKUP;
         }
         uint32_t sizeImm = 0u;
         if (opcode == spv::Op::OpCopyMemorySized) {
            const auto sizeId = spirv::getOperand<spv::Id>(parsedInstruction, 2u);
            auto size = spvValues.find(sizeId);
            if (size == spvValues.end()) {
               _debug_printf("Couldn't find size with id %u\n", sizeId);
               return SPV_ERROR_INVALID_LOOKUP;
            }
            assert(size->second.storageFile == SpirvFile::IMMEDIATE);
            sizeImm = (info->target < 0xc0) ? size->second.value[0u].value->reg.data.u32 : size->second.value[0u].value->reg.data.u64;
         } else {
            sizeImm = reinterpret_cast<const TypePointer *>(target->second.type)->getPointedType()->getSize();
         }
         const auto targetStorage = target->second.storageFile;
         const auto sourceStorage = source->second.storageFile;

         const bool isPacked = target->second.isPacked;

         if (targetStorage == SpirvFile::TEMPORARY && (sourceStorage == SpirvFile::TEMPORARY || sourceStorage == SpirvFile::IMMEDIATE)) {
            for (unsigned int i = 0, c = 0u; i < sizeImm && c < target->second.value.size(); i += typeSizeof(target->second.value[c].value->reg.type), ++c) {
               assert(target->second.value[c].value->reg.size == source->second.value[c].value->reg.size);
               i += source->second.paddings[c];
               mkMov(target->second.value[c].value, source->second.value[c].value, source->second.value[c].value->reg.type);
            }
         } else if (targetStorage == SpirvFile::TEMPORARY) {
            // FIXME load to reg of size alignment, then split things up
            for (unsigned int i = 0, c = 0u; i < sizeImm && c < target->second.value.size(); i += alignment, ++c) {
               auto const offsetImm = mkImm(i);
               Value *offset = getScratch(4u);
               mkMov(offset, offsetImm, TYPE_U32);
               mkLoad(target->second.value[c].value->reg.type, target->second.value[c].value, source->second.value[0u].symbol, offset);
            }
         } else if (sourceStorage == SpirvFile::TEMPORARY || sourceStorage == SpirvFile::IMMEDIATE) {
            if (true /* packed */) {
               unsigned int processedSize = 0u, c = 0u;
               PValue storePointer = target->second.value.front();
               std::stack<const Type *> typesStack;
               const Type *pointedType = reinterpret_cast<const TypePointer *>(source->second.type)->getPointedType();
               for (unsigned int i = 0u; i < sizeImm; i += pointedType->getSize())
                  typesStack.push(pointedType);
               while (processedSize < sizeImm && c < source->second.value.size()) {
                  const auto currentType = typesStack.top();
                  Value *object = source->second.value[c].value;
                  typesStack.pop();
                  if (currentType->isCompooundType()) {
                     for (unsigned int i = currentType->getElementsNb(); i > 0u; --i)
                        typesStack.push(currentType->getElementType(i - 1u));
                     continue;
                  }
                  const auto objectType = object->reg.type;
                  const auto typeSize = (objectType != TYPE_NONE) ? typeSizeof(objectType) : currentType->getSize();

                  // XXX temporary, should not be used with packed
                  processedSize += source->second.paddings[c];

                  auto const split4Byte = [&](Value *value, unsigned int offset){
                     Value *tmp = getScratch(4u);
                     Value *andImm = getScratch(4u);
                     mkMov(andImm, mkImm(0xff), TYPE_U32);
                     mkOp2(OP_AND, TYPE_U32, tmp, value, andImm);
                     store(targetStorage, storePointer, offset + processedSize, tmp, TYPE_U8, access, alignment);
                     Value *shrImm8 = getScratch(4u);
                     mkMov(shrImm8, mkImm(0x8), TYPE_U32);
                     mkOp2(OP_SHR, TYPE_U32, tmp, value, shrImm8);
                     mkOp2(OP_AND, TYPE_U32, tmp, tmp, andImm);
                     store(targetStorage, storePointer, offset + processedSize + 1u, tmp, TYPE_U8, access, alignment);
                     Value *shrImm10 = getScratch(4u);
                     mkMov(shrImm10, mkImm(0x10), TYPE_U32);
                     mkOp2(OP_SHR, TYPE_U32, tmp, value, shrImm10);
                     mkOp2(OP_AND, TYPE_U32, tmp, tmp, andImm);
                     store(targetStorage, storePointer, offset + processedSize + 2u, tmp, TYPE_U8, access, alignment);
                     Value *shrImm18 = getScratch(4u);
                     mkMov(shrImm18, mkImm(0x18), TYPE_U32);
                     mkOp2(OP_SHR, TYPE_U32, tmp, value, shrImm18);
                     store(targetStorage, storePointer, offset + processedSize + 3u, tmp, TYPE_U8, access, alignment);
                  };

                  if (typeSize == 1u) {
                     store(targetStorage, storePointer, processedSize, object, TYPE_U8, access, alignment);
                  } else if (typeSize == 2u) {
                     Value *tmp = getScratch(4u);
                     Value *andImm = getScratch(4u);
                     mkMov(andImm, mkImm(0xff), TYPE_U32);
                     mkOp2(OP_AND, TYPE_U32, tmp, object, andImm);
                     store(targetStorage, storePointer, processedSize, tmp, TYPE_U8, access, alignment);
                     Value *shrImm = getScratch(4u);
                     mkMov(shrImm, mkImm(0x8), TYPE_U32);
                     mkOp2(OP_SHR, TYPE_U32, tmp, object, shrImm);
                     store(targetStorage, storePointer, processedSize + 1u, tmp, TYPE_U8, access, alignment);
                  } else if (typeSize == 4u) {
                     split4Byte(object, 0u);
                  } else if (typeSize == 8u) {
                     Value *splits[2];
                     mkSplit(splits, 4u, object);
                     split4Byte(splits[0], 0u);
                     split4Byte(splits[1], 4u);
                  } else {
                     assert(false);
                  }
                  processedSize += typeSize;
                  ++c;
               }
            } else {
               // TODO
            }
         } else {
            _debug_printf("Unsupported copy setup\n");
            return SPV_UNSUPPORTED;
         }
      }
      break;
   case spv::Op::OpIEqual:
   case spv::Op::OpFOrdEqual:
   case spv::Op::OpINotEqual:
   case spv::Op::OpFOrdNotEqual:
   case spv::Op::OpSGreaterThan:
   case spv::Op::OpUGreaterThan:
   case spv::Op::OpFOrdGreaterThan:
   case spv::Op::OpFUnordGreaterThan:
   case spv::Op::OpSGreaterThanEqual:
   case spv::Op::OpUGreaterThanEqual:
   case spv::Op::OpFOrdGreaterThanEqual:
   case spv::Op::OpFUnordGreaterThanEqual:
   case spv::Op::OpSLessThan:
   case spv::Op::OpULessThan:
   case spv::Op::OpFOrdLessThan:
   case spv::Op::OpFUnordLessThan:
   case spv::Op::OpSLessThanEqual:
   case spv::Op::OpULessThanEqual:
   case spv::Op::OpFOrdLessThanEqual:
   case spv::Op::OpFUnordLessThanEqual:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1Struct = getStructForOperand(2u);
         const SpirVValue &op2Struct = getStructForOperand(3u);

         const Type *op1Type = op1Struct.type;

         const int _isSrcSigned = isSrcSigned(opcode);
         const DataType srcTy = (op1Type->getElementsNb() == 1u) ? op1Type->getEnumType(_isSrcSigned)
                                                                 : op1Type->getElementEnumType(0u, _isSrcSigned);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *op1 = op1Struct.getValue(this, i);
            Value *op2 = op2Struct.getValue(this, i);

            Value *predicate = getScratch(1, FILE_PREDICATE);
            mkCmp(OP_SET, convertCc(opcode), TYPE_U32, predicate, srcTy, op1, op2);
            values.emplace_back(predicate);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpSNegate:
   case spv::Op::OpFNegate:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &opStruct = getStructForOperand(2u);
         const DataType dstTy = (resType->getElementsNb() == 1u) ? resType->getEnumType()
                                                                 : resType->getElementEnumType(0u);
         const unsigned int elemByteSize = typeSizeof(dstTy);
         const operation op = convertOp(opcode);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i)
            values.emplace_back(mkOp1v(op, dstTy, getScratch(elemByteSize), opStruct.getValue(this, i)));

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpIAdd:
   case spv::Op::OpFAdd:
   case spv::Op::OpISub:
   case spv::Op::OpFSub:
   case spv::Op::OpIMul:
   case spv::Op::OpFMul:
   case spv::Op::OpSDiv:
   case spv::Op::OpUDiv:
   case spv::Op::OpFDiv:
   case spv::Op::OpSMod:
   case spv::Op::OpUMod:
   case spv::Op::OpFMod:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1Struct = getStructForOperand(2u);
         const SpirVValue &op2Struct = getStructForOperand(3u);

         const int _isSrcSigned = isSrcSigned(opcode);
         const DataType dstTy = (resType->getElementsNb() == 1u) ? resType->getEnumType(_isSrcSigned)
                                                                 : resType->getElementEnumType(0u, _isSrcSigned);
         const unsigned int elemByteSize = typeSizeof(dstTy);
         const operation op = convertOp(opcode);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *op1 = op1Struct.getValue(this, i);
            Value *op2 = op2Struct.getValue(this, i);

            values.emplace_back(mkOp2v(op, dstTy, getScratch(elemByteSize), op1, op2));
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpSRem:
   case spv::Op::OpFRem:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1Struct = getStructForOperand(2u);
         const SpirVValue &op2Struct = getStructForOperand(3u);

         const int _isSrcSigned = isSrcSigned(opcode);
         const DataType dstTy = (resType->getElementsNb() == 1u) ? resType->getEnumType(_isSrcSigned)
                                                                 : resType->getElementEnumType(0u, _isSrcSigned);
         const unsigned int elemByteSize = typeSizeof(dstTy);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *op1 = op1Struct.getValue(this, i);
            Value *op2 = op2Struct.getValue(this, i);

            Value *tmp1 = mkOp2v(OP_DIV, dstTy, getScratch(elemByteSize), op1, op2);
            Value *tmp2 = mkOp2v(OP_MUL, dstTy, getScratch(elemByteSize), op2, tmp1);
            Value *res = mkOp2v(OP_SUB, dstTy, getScratch(elemByteSize), op1, tmp2);

            values.emplace_back(res);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   // TODO(pmoreau): Make use of the scope and memory semantics
   // TODO(pmoreau): Need to check those opcodes again
   case spv::Op::OpAtomicExchange:
   case spv::Op::OpAtomicIIncrement:
   case spv::Op::OpAtomicIDecrement:
   case spv::Op::OpAtomicIAdd:
   case spv::Op::OpAtomicISub:
   case spv::Op::OpAtomicSMin:
   case spv::Op::OpAtomicUMin:
   case spv::Op::OpAtomicSMax:
   case spv::Op::OpAtomicUMax:
   case spv::Op::OpAtomicAnd:
   case spv::Op::OpAtomicOr:
   case spv::Op::OpAtomicXor:
      {
         const bool hasNoValue = (opcode == spv::Op::OpAtomicIIncrement) || (opcode == spv::Op::OpAtomicIDecrement);

         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &pointerStruct = getStructForOperand(2u);
         Value *pointer = pointerStruct.value.front().value;
         Value *value = hasNoValue ? nullptr : getStructForOperand(5u).value.front().value;

         const int _isSrcSigned = isSrcSigned(opcode);

         if (opcode == spv::Op::OpAtomicIDecrement) {
            value = getScratch(resType->getSize());
            mkMov(value, mkImm(-1), resType->getEnumType(_isSrcSigned));
         }

         Value *res = getScratch(resType->getSize());
         Value *base = acquire(SpirvFile::GLOBAL, pointerStruct.type);
         Instruction *insn = opcode == spv::Op::OpAtomicIIncrement ? mkOp1(OP_ATOM, resType->getEnumType(_isSrcSigned), res, base)
                                                                   : mkOp2(OP_ATOM, resType->getEnumType(_isSrcSigned), res, base, value);
         insn->subOp = getSubOp(opcode);
         insn->setIndirect(0, 0, pointer);
         if (opcode == spv::Op::OpAtomicISub)
            insn->src(1).mod.neg();

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, { res }, resType->getPaddings() });
      }
      break;
   // TODO(pmoreau): Make use of the scope and memory semantics
   case spv::Op::OpAtomicCompareExchange:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &pointerStruct = getStructForOperand(2u);
         Value *pointer = pointerStruct.value.front().value;
         Value *value = getStructForOperand(6u).value.front().value;
         Value *comparator = getStructForOperand(7u).value.front().value;

         Value *res = getScratch(resType->getSize());
         auto base = acquire(SpirvFile::GLOBAL, pointerStruct.type);
         Instruction *insn = mkOp3(OP_ATOM, resType->getEnumType(), res, base, value, comparator);
         insn->subOp = getSubOp(opcode);
         insn->setIndirect(0, 0, pointer);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, { res }, resType->getPaddings() });
      }
      break;
   case spv::Op::OpShiftLeftLogical:
   case spv::Op::OpShiftRightLogical:
   case spv::Op::OpShiftRightArithmetic:
   case spv::Op::OpBitwiseOr:
   case spv::Op::OpBitwiseXor:
   case spv::Op::OpBitwiseAnd:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1 = getStructForOperand(2u);
         const SpirVValue &op2 = getStructForOperand(3u);

         const operation op = convertOp(opcode);
         const Type *elementType = resType->getElementsNb() == 1u ? resType : resType->getElementType(0u);
         int isSigned = 0;
         if (opcode == spv::Op::OpShiftRightArithmetic)
            isSigned = 1;
         const DataType dstTy = elementType->getEnumType(isSigned);
         const unsigned int elemByteSize = elementType->getSize();

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *res = mkOp2v(op, dstTy, getScratch(elemByteSize), op1.getValue(this, i), op2.getValue(this, i));
            values.emplace_back(res);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });

      }
      break;
   case spv::Op::OpVectorTimesScalar:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1 = getStructForOperand(2u);
         const SpirVValue &op2 = getStructForOperand(3u);
         const DataType dstTy = resType->getElementEnumType(0u);
         const unsigned int elemByteSize = resType->getElementSize(0u);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *res = mkOp2v(OP_MUL, dstTy, getScratch(elemByteSize), op1.getValue(this, i), op2.getValue(this, 0u));
            values.emplace_back(res);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpVectorShuffle:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op1 = getStructForOperand(2u);
         const SpirVValue &op2 = getStructForOperand(3u);
         const unsigned int op1ElemNb = op1.type->getElementsNb();

         std::vector<PValue> values;
         values.reserve(parsedInstruction->num_operands - 4u);
         for (uint16_t i = 4u; i < parsedInstruction->num_operands; ++i) {
            const word componentIndex = spirv::getOperand<word>(parsedInstruction, i);
            if (componentIndex == UINT32_MAX) {
               values.emplace_back(getScratch(std::max(4u, resType->getElementSize(i - 4u))));
               continue;
            }

            Value *src = componentIndex < op1ElemNb ? op1.value[componentIndex].value
                                                    : op2.value[componentIndex - op1ElemNb].value;
            auto regSize = std::max(static_cast<uint8_t>(4u), src->reg.size);
            Value *dst = getScratch(regSize);
            mkMov(dst, src, typeOfSize(regSize));
            values.emplace_back(dst);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpUConvert:
   case spv::Op::OpSConvert:
   case spv::Op::OpConvertUToF:
   case spv::Op::OpConvertFToU:
   case spv::Op::OpConvertSToF:
   case spv::Op::OpConvertFToS:
   case spv::Op::OpConvertPtrToU:
   case spv::Op::OpSatConvertSToU:
   case spv::Op::OpSatConvertUToS:
   case spv::Op::OpConvertUToPtr:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &src = getStructForOperand(2u);

         const int _isSrcSigned = isSrcSigned(opcode);
         const int _isDstSigned = isDstSigned(opcode);

         int saturate = opcode == spv::Op::OpSatConvertSToU || opcode == spv::Op::OpSatConvertUToS;

         const unsigned int elemByteSize = std::max(4u, resType->getElementSize(0u));
         const DataType dstTy = resType->getElementEnumType(0u, _isDstSigned);
         const DataType srcTy = src.type->getElementEnumType(0u, _isSrcSigned);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());
         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *res = getScratch(elemByteSize);
            if (opcode == spv::Op::OpConvertPtrToU && !src.value[i].isValue()) {
               mkMov(res, mkImm(src.value[i].symbol->reg.data.offset), dstTy);
               if (src.value[i].indirect != nullptr)
                  mkOp2(OP_ADD, dstTy, res, res, src.value[i].indirect);
            } else {
               mkCvt(OP_CVT, dstTy, res, srcTy, src.getValue(this, i))->saturate = saturate;
            }
            values.emplace_back(res);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   case spv::Op::OpControlBarrier:
      {
         const ImmediateValue *executionImm = getStructForOperand(0u).value.front().value->asImm();
         const spv::Scope execution = static_cast<spv::Scope>(executionImm->reg.data.u32);

         spv_result_t res = generateCtrlBarrier(execution);
         if (res != SPV_SUCCESS)
            return res;

         const ImmediateValue *memoryImm = getStructForOperand(1u).value.front().value->asImm();
         const ImmediateValue *memorySemanticsImm = getStructForOperand(2u).value.front().value->asImm();
         const spv::Scope memory = static_cast<spv::Scope>(memoryImm->reg.data.u32);
         const spv::MemorySemanticsMask memorySemantics = static_cast<spv::MemorySemanticsMask>(memorySemanticsImm->reg.data.u32);

         if (memorySemantics != spv::MemorySemanticsMask::MaskNone) {
            res = generateMemBarrier(memory, memorySemantics);
            if (res != SPV_SUCCESS)
               return res;
         }
      }
      break;
   case spv::Op::OpMemoryBarrier:
      {
         const ImmediateValue *memoryImm = getStructForOperand(0u).value.front().value->asImm();
         const ImmediateValue *memorySemanticsImm = getStructForOperand(1u).value.front().value->asImm();

         const spv::Scope memory = static_cast<spv::Scope>(memoryImm->reg.data.u32);
         const spv::MemorySemanticsMask memorySemantics = static_cast<spv::MemorySemanticsMask>(memorySemanticsImm->reg.data.u32);

         const spv_result_t res = generateMemBarrier(memory, memorySemantics);
         if (res != SPV_SUCCESS)
            return res;
      }
      break;
   case spv::Op::OpSampledImage:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const Image &image = images.find(getIdOfOperand(2u))->second;
         const Sampler &sampler = samplers.find(getIdOfOperand(3u))->second;

         sampledImages.emplace(resId, SampledImage{ reinterpret_cast<TypeSampledImage const*>(resType), image, sampler });
      }
      break;
   case spv::Op::OpImageSampleExplicitLod:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SampledImage& sampledImage = sampledImages.find(getIdOfOperand(2u))->second;
         const SpirVValue& coordinates = getStructForOperand(3u);
         const auto operand = spirv::getOperand<spv::ImageOperandsMask>(parsedInstruction, 4u);

         unsigned int operandIndex = 5u;
         Value *bias = nullptr;
         Value *lod = nullptr;
         Value *gradDx = nullptr, *gradDy = nullptr;
         Value *constOffset = nullptr;
         Value *offset = nullptr;
         Value *constOffsets = nullptr;
         Value *sample = nullptr;
         Value *minLod = nullptr;
         if (hasFlag(operand, spv::ImageOperandsShift::Bias))
            bias = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::Lod))
            lod = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::Grad)) {
            gradDx = getOp(getIdOfOperand(operandIndex++)).value;
            gradDy = getOp(getIdOfOperand(operandIndex++)).value;
         }
         if (hasFlag(operand, spv::ImageOperandsShift::ConstOffset))
            constOffset = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::Offset))
            offset = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::ConstOffsets))
            constOffsets = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::Sample))
            sample = getOp(getIdOfOperand(operandIndex++)).value;
         if (hasFlag(operand, spv::ImageOperandsShift::MinLod))
            minLod = getOp(getIdOfOperand(operandIndex++)).value;

         const bool specifyLod = lod != nullptr && false;

         auto const componentSize = resType->getElementType(0u)->getSize();
         std::vector<PValue> res = { getScratch(componentSize), getScratch(componentSize), getScratch(componentSize), getScratch(componentSize) };
         std::vector<Value*> resValue;
         for (auto &i : res)
            resValue.push_back(i.value);

         const Image& image = sampledImage.image;
         const Sampler& sampler = sampledImage.sampler;
         const auto imageTarget = getTexTarget(image.type);
         const auto tic = image.index;
         const auto tsc = sampler.index;

         std::vector<Value*> args;
         for (auto &i : coordinates.value)
            args.push_back(i.value);
         if (specifyLod)
            args.push_back(lod);

         auto ld = mkTex(OP_TEX, imageTarget, tic, tsc, resValue, args);
         ld->tex.levelZero = !specifyLod;
         ld->tex.mask = (1u << resValue.size()) - 1u;
         ld->tex.format = getImageFormat(image.type->format);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, res, { 1u } });
      }
      break;
   case spv::Op::OpImageQuerySize:
   case spv::Op::OpImageQuerySizeLod:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const Image& image = images.find(getIdOfOperand(2u))->second;
         const TypeImage *imageType = image.type;
         const auto imageTarget = getTexTarget(imageType);

         const auto componentSize = resType->getElementsNb() == 1u ? resType->getSize() : resType->getElementSize(0u);
         std::vector<PValue> res;
         switch (imageType->dim) {
            case spv::Dim::Dim3D:
               res.push_back(getScratch(componentSize));
               // FALLTHROUGH
            case spv::Dim::Dim2D:
            case spv::Dim::Cube:
               res.push_back(getScratch(componentSize));
               // FALLTHROUGH
            case spv::Dim::Dim1D:
               res.push_back(getScratch(componentSize));
               break;
         }
         if (imageType->arrayed)
            res.push_back(getScratch(componentSize));
         std::vector<Value*> resValue;
         for (auto &i : res)
            resValue.push_back(i.value);

         Value *lod = opcode == spv::Op::OpImageQuerySizeLod ? getOp(getIdOfOperand(3u)).value : mkImm(0x0);
         std::vector<Value*> args = { lod };
         const auto tic = image.index;
         auto ld = mkTex(OP_TXQ, imageTarget, tic, 0, resValue, args);
         ld->tex.mask = (1u << resValue.size()) - 1u;
         ld->tex.query = TexQuery::TXQ_DIMS;

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, res, std::vector<uint32_t>(componentSize, res.size()) });
      }
      break;
   case spv::Op::OpIsInf:
   case spv::Op::OpIsNan:
   case spv::Op::OpIsFinite:
   case spv::Op::OpIsNormal:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;
         const SpirVValue &op = getStructForOperand(2u);
         const DataType dstTy = (resType->getElementsNb() == 1u) ? resType->getEnumType()
                                                                 : resType->getElementEnumType(0u);
         const unsigned int elemByteSize = typeSizeof(dstTy);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());

         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            DataType sType = op.type->getElementEnumType(i);
            unsigned int sTypeSize = typeSizeof(sType);
            DataType siType = typeOfSize(sTypeSize, false, false);
            Value *src = op.getValue(this, i);
            Value *tmp = getScratch(sTypeSize);
            Value *pred = getScratch(1, FILE_PREDICATE);

            CondCode cc;
            switch(opcode) {
            case spv::Op::OpIsInf:
               cc = CC_EQ;
               break;
            case spv::Op::OpIsNan:
               cc = CC_GT;
               break;
            case spv::Op::OpIsNormal:
            case spv::Op::OpIsFinite:
               cc = CC_LT;
               break;
            }

            if (sType == TYPE_F64) {
               mkOp2(OP_AND, siType, tmp, src, loadImm(getScratch(sTypeSize), 0x7fffffffffffffffUL));
               mkCmp(OP_SET, cc, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x7ff0000000000000UL));
               if (opcode == spv::Op::OpIsNormal) {
                  mkCmp(OP_SET_AND, CC_GE, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x0010000000000000UL), pred);
                  mkCmp(OP_SET_OR, CC_EQ, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x0UL), pred);
               }
            } else {
               mkOp2(OP_AND, siType, tmp, src, loadImm(getScratch(sTypeSize), 0x7fffffff));
               mkCmp(OP_SET, cc, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x7f800000));
               if (opcode == spv::Op::OpIsNormal) {
                  mkCmp(OP_SET_AND, CC_GE, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x00800000), pred);
                  mkCmp(OP_SET_OR, CC_EQ, siType, pred, siType, tmp, loadImm(getScratch(sTypeSize), 0x0), pred);
               }
            }
            values.emplace_back(pred);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   // TODO: aggregate types and booleans.
   case spv::Op::OpSelect:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const SpirVValue &opC = getStructForOperand(2u);
         const SpirVValue &op0 = getStructForOperand(3u);
         const SpirVValue &op1 = getStructForOperand(4u);

         const DataType dstTy = (resType->getElementsNb() == 1u) ? resType->getEnumType()
                                                                 : resType->getElementEnumType(0u);
         const unsigned int elemByteSize = typeSizeof(dstTy);

         std::vector<PValue> values;
         values.reserve(resType->getElementsNb());

         for (unsigned int i = 0u; i < resType->getElementsNb(); ++i) {
            Value *srcC = opC.getValue(this, i);
            Value *src0 = op0.getValue(this, i);
            Value *src1 = op1.getValue(this, i);
            Value *dst  = getScratch(src0->reg.size, src0->reg.file);

            mkOp3(OP_SELP, dstTy, dst, src0, src1, srcC);

            values.emplace_back(dst);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, resType, values, resType->getPaddings() });
      }
      break;
   // TODO(pmoreau):
   // * Fix handling of events
   // * Fix handling of vectors
   case spv::Op::OpGroupAsyncCopy:
      {
         const Type *resType = types.find(parsedInstruction->type_id)->second;
         const spv::Id resId = parsedInstruction->result_id;

         const SpirVValue &dst = getStructForOperand(3u);
         const SpirVValue &src = getStructForOperand(4u);

         Value *numElements = getOp(getIdOfOperand(5u)).value;
         mkMov(numElements, mkImm(0x1), TYPE_U64);
         Value *stride = getOp(getIdOfOperand(6u)).value;

         uint32_t regSize = numElements->reg.size;
         DataType regType = (info->target < 0xc0) ? TYPE_U32 : TYPE_U64;

         const PValue &srcPtr = src.value[0u];
         Symbol *srcSym = srcPtr.symbol;
         if (srcSym == nullptr)
            srcSym = createSymbol(src.storageFile, regType, regSize, 0u);

         const PValue &dstPtr = dst.value[0u];
         Symbol *dstSym = dstPtr.symbol;
         if (dstSym == nullptr)
            dstSym = createSymbol(dst.storageFile, regType, regSize, 0u);

         const DataType typeEnum = reinterpret_cast<const TypePointer*>(dst.type)->getPointedType()->getEnumType();
         const uint32_t typeSize = reinterpret_cast<const TypePointer*>(dst.type)->getPointedType()->getSize();
         Value *typeSizeImm = getScratch(regSize);
         mkMov(typeSizeImm, mkImm(typeSize), regType);

         Value *srcDelta = getScratch(regSize);
         Value *dstDelta = getScratch(regSize);
         if (dst.storageFile == SpirvFile::GLOBAL) {
            mkMov(srcDelta, stride,     regType);
            mkMov(dstDelta, mkImm(0x1), regType);
         } else {
            mkMov(srcDelta, mkImm(0x1), regType);
            mkMov(dstDelta, stride,     regType);
         }

         auto getSysVal = [this](SVSemantic svName, uint32_t index){
            return mkOp1v(OP_RDSV, TYPE_U32, getScratch(), mkSysVal(svName, index));
         };

         Value *tid = getScratch(4u);
         mkMov(tid, getSysVal(SV_TID, 0u), TYPE_U32);
         //mkOp3(OP_MAD, TYPE_U32, tid, getSysVal(SV_NTID, 1u), getSysVal(SV_TID, 2u), getSysVal(SV_TID, 1u));
         //mkOp3(OP_MAD, TYPE_U32, tid, getSysVal(SV_NTID, 0u), tid, getSysVal(SV_TID, 0u));
         if (regSize == 8u) {
            Value *tmp = getScratch(8u);
            mkCvt(OP_CVT, TYPE_U64, tmp, TYPE_U32, tid);
            tid = tmp;
         }
         Value *blockSize = getScratch(4u);
         mkMov(blockSize, getSysVal(SV_NTID, 0u), TYPE_U32);
         //mkOp2(OP_MUL, TYPE_U32, blockSize, getSysVal(SV_NTID, 0u), getSysVal(SV_NTID, 1u));
         //mkOp2(OP_MUL, TYPE_U32, blockSize, blockSize, getSysVal(SV_NTID, 2u));
         if (regSize == 8u) {
            Value *tmp = getScratch(8u);
            mkCvt(OP_CVT, TYPE_U64, tmp, TYPE_U32, blockSize);
            blockSize = tmp;
         }
         Value *srcByteStride = getScratch(regSize);
         mkOp2(OP_MUL, regType, srcByteStride, typeSizeImm, srcDelta);
         Value *dstByteStride = getScratch(regSize);
         mkOp2(OP_MUL, regType, dstByteStride, typeSizeImm, dstDelta);

         Value *srcIndirect = getScratch(regSize);
         mkOp3(OP_MAD, regType, srcIndirect, tid, srcByteStride, srcPtr.indirect);
         Value *dstIndirect = getScratch(regSize);
         mkOp3(OP_MAD, regType, dstIndirect, tid, dstByteStride, dstPtr.indirect);

         if (regSize == 8u) {
            Value *tmps[2];
            mkSplit(tmps, 4u, srcIndirect);
            mkOp2(OP_MERGE, TYPE_U64, srcIndirect, tmps[0], tmps[1]);
            mkSplit(tmps, 4u, dstIndirect);
            mkOp2(OP_MERGE, TYPE_U64, dstIndirect, tmps[0], tmps[1]);
         }

         Value *srcDeltaByteStride = getScratch(regSize);
         mkOp2(OP_MUL, regType, srcDeltaByteStride, blockSize, srcByteStride);
         Value *dstDeltaByteStride = getScratch(regSize);
         mkOp2(OP_MUL, regType, dstDeltaByteStride, blockSize, dstByteStride);

         Value *iter = getScratch(regSize);
         mkMov(iter, tid, regType);

         BasicBlock *headerBB = new BasicBlock(func);
         bb->cfg.attach(&headerBB->cfg, Graph::Edge::TREE);
         setPosition(headerBB, true);

         BasicBlock *mergeBB = new BasicBlock(func);
         BasicBlock *loopBB = new BasicBlock(func);

         Value *pred = getScratch(1, FILE_PREDICATE);
         mkCmp(OP_SET, CC_GE, TYPE_U32, pred, regType, iter, numElements);
         mkFlow(OP_BRA, mergeBB, CC_P, pred);
         bb->cfg.attach(&loopBB->cfg, Graph::Edge::TREE);
         bb->cfg.attach(&mergeBB->cfg, Graph::Edge::TREE);
         setPosition(loopBB, true);

         Value *tmpValue = getScratch(std::max(4u, typeSize));
         mkLoad(typeEnum, tmpValue, srcSym, srcIndirect);
         mkStore(OP_STORE, typeEnum, dstSym, dstIndirect, tmpValue);

         mkOp2(OP_ADD, regType, iter, iter, blockSize);
         mkOp2(OP_ADD, regType, srcIndirect, srcIndirect, srcDeltaByteStride);
         mkOp2(OP_ADD, regType, dstIndirect, dstIndirect, dstDeltaByteStride);
         mkFlow(OP_BRA, headerBB, CC_ALWAYS, nullptr);
         bb->cfg.attach(&headerBB->cfg, Graph::Edge::BACK);
         setPosition(mergeBB, true);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::NONE, resType, { nullptr }, { } });
      }
      break;
   // TODO(pmoreau): Handle different events
   case spv::Op::OpGroupWaitEvents:
      {
         const ImmediateValue *executionImm = getStructForOperand(0u).value.front().value->asImm();
         const spv::Scope execution = static_cast<spv::Scope>(executionImm->reg.data.u32);
         spv_result_t res = generateCtrlBarrier(execution);
         if (res != SPV_SUCCESS)
            return res;
      }
      break;
   default:
      _debug_printf("Unsupported opcode %u\n", opcode);
      return SPV_UNSUPPORTED;
   }

   return SPV_SUCCESS;
}

spv_result_t
Converter::convertEntryPoint(const spv_parsed_instruction_t *parsedInstruction)
{
   EntryPoint entryPoint;
   entryPoint.index = static_cast<uint32_t>(entryPoints.size());
   entryPoint.executionModel = spirv::getOperand<spv::ExecutionModel>(parsedInstruction, 0u);
   entryPoint.name = spirv::getOperand<const char*>(parsedInstruction, 2u);
   entryPoint.interface.reserve(parsedInstruction->num_operands - 3u);

   for (unsigned int i = 3u; i < parsedInstruction->num_operands; ++i)
      entryPoint.interface.push_back(spirv::getOperand<spv::Id>(parsedInstruction, i));

   const spv::Id id = spirv::getOperand<spv::Id>(parsedInstruction, 1u);
   entryPoints.emplace(id, entryPoint);
   names.emplace(id, entryPoint.name);

   return SPV_SUCCESS;
}

spv_result_t
Converter::convertDecorate(const spv_parsed_instruction_t *parsedInstruction, bool hasMember)
{
   assert(!hasMember);
   const unsigned int offset = hasMember ? 1u : 0u;

   Words literals = Words();
   for (unsigned int i = 3u + offset; i < parsedInstruction->num_words; ++i)
      literals.push_back(parsedInstruction->words[i]);
   decorations[spirv::getOperand<spv::Id>(parsedInstruction, 0u)][spirv::getOperand<spv::Decoration>(parsedInstruction, 1u + offset)].emplace_back(literals);

   return SPV_SUCCESS;
}

spv_result_t
Converter::loadBuiltin(spv::Id dstId, Type const* dstType, Words const& decLiterals, spv::MemoryAccessMask access)
{
   auto const builtin = static_cast<spv::BuiltIn>(decLiterals[0u]);

   auto getSysVal = [this](SVSemantic svName, uint32_t index){
      return mkOp1v(OP_RDSV, TYPE_U32, getScratch(), mkSysVal(svName, index));
   };

   std::function<Value *(unsigned int)> vec3Func;
   switch (builtin) {
   case spv::BuiltIn::WorkDim:
      vec3Func = [getSysVal,this](unsigned int index){ return getSysVal(SV_WORK_DIM, index); };
      break;
   case spv::BuiltIn::LocalInvocationId:
      vec3Func = [getSysVal,this](unsigned int index){ return getSysVal(SV_TID, index); };
      break;
   case spv::BuiltIn::NumWorkgroups:
      vec3Func = [getSysVal,this](unsigned int index){ return getSysVal(SV_NCTAID, index); };
      break;
   case spv::BuiltIn::WorkgroupSize:
      vec3Func = [getSysVal,this](unsigned int index){ return getSysVal(SV_NTID, index); };
      break;
   case spv::BuiltIn::WorkgroupId:
      vec3Func = [getSysVal,this](unsigned int index){ return getSysVal(SV_CTAID, index); };
      break;
   case spv::BuiltIn::GlobalInvocationId:
      vec3Func = [getSysVal,this](unsigned int index){
         // FIXME: the proper formula is (ntid * ctaid) + tid + gid_off
         return mkOp3v(OP_MAD,  TYPE_U32, getScratch(), getSysVal(SV_NTID, index), getSysVal(SV_CTAID, index), getSysVal(SV_TID, index));
      };
      break;
   case spv::BuiltIn::GlobalSize:
      vec3Func = [getSysVal,this](unsigned int index){
         return mkOp2v(OP_MUL, TYPE_U32, getScratch(), getSysVal(SV_NTID, index), getSysVal(SV_NCTAID, index));
      };
      break;
   default:
      break;
   }

   auto const& type = dstType->getElementType(0u);
   auto const typeEnum = type->getEnumType();
   auto const typeSize = type->getSize();

   switch (builtin) {
   case spv::BuiltIn::WorkDim:
   case spv::BuiltIn::LocalInvocationId:
   case spv::BuiltIn::NumWorkgroups:
   case spv::BuiltIn::WorkgroupSize:
   case spv::BuiltIn::WorkgroupId:
   case spv::BuiltIn::GlobalInvocationId:
   case spv::BuiltIn::GlobalSize:
      {
         std::vector<PValue> values = { vec3Func(0u), vec3Func(1u), vec3Func(2u) };
         for (PValue &value : values) {
            const DataType builtinEnum = TYPE_U32;
            if (builtinEnum == typeEnum)
               continue;
            Value *res = getScratch(typeSize);
            mkCvt(OP_CVT, typeEnum, res, builtinEnum, value.value);
            value.value = res;
         }
         spvValues.emplace(dstId, SpirVValue{ SpirvFile::TEMPORARY, dstType, values, { 1u, 1u, 1u } });
      }
      break;
   default:
      _debug_printf("Unsupported builtin %u\n", builtin);
      return SPV_UNSUPPORTED;
   }

   return SPV_SUCCESS;
}

spv_result_t
Converter::convertOpenCLInstruction(spv::Id resId, Type const* type, OpenCLLIB::Entrypoints op, const spv_parsed_instruction_t *parsedInstruction)
{
   auto getOp = [&](spv::Id id, unsigned c = 0u){
      auto searchOp = spvValues.find(id);
      if (searchOp == spvValues.end())
         return static_cast<Value*>(nullptr);

      auto& opStruct = searchOp->second;
      if (c >= opStruct.value.size()) {
         _debug_printf("Trying to access element %u out of %u\n", c, opStruct.value.size());
         return static_cast<Value*>(nullptr);
      }

      auto op = opStruct.value[c].value;
      if (opStruct.storageFile == SpirvFile::IMMEDIATE) {
         auto constant = op;
         op = getScratch(constant->reg.size);
         mkMov(op, constant, constant->reg.type);
      }
      return op;
   };

   switch (op) {
   case OpenCLLIB::Prefetch:
      {
         _debug_printf("Unsupported OpenCLLIB opcode %u\n", op);
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Fmax:
   case OpenCLLIB::Fmin:
      {
         operation opcode;
         switch (op) {
         case OpenCLLIB::Fmax: opcode = OP_MAX; break;
         case OpenCLLIB::Fmin: opcode = OP_MIN; break;
         }

         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            DataType dType = type->getElementEnumType(i);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);

            mkOp2(opcode, dType, res, op1, op2);

            values.push_back(res);
         }

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Nextafter:
      {
         // TODO: fix nextafter(0, -1)
         auto op1 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 4u), 0u);
         auto op2 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 5u), 0u);
         auto tmp = getScratch();
         auto res = getScratch();
         auto pred = getScratch(1, FILE_PREDICATE);

         mkCmp(OP_SLCT, CC_GE, TYPE_S32, tmp, TYPE_F32, loadImm(getScratch(), 1), loadImm(getScratch(), -1), op1);

         BasicBlock *tBB = new BasicBlock(func);
         BasicBlock *fBB = new BasicBlock(func);
         BasicBlock *endBB = new BasicBlock(func);

         bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
         bb->cfg.attach(&tBB->cfg, Graph::Edge::TREE);
         mkCmp(OP_SET, CC_GT, TYPE_U8, pred, TYPE_F32, op2, op1);
         mkFlow(OP_BRA, tBB, CC_P, pred);

         setPosition(fBB, true);
         fBB = new BasicBlock(func);
         bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
         mkFlow(OP_BRA, fBB, CC_ALWAYS, nullptr);

         setPosition(tBB, true);
         mkOp2(OP_ADD, TYPE_S32, res, op1, tmp);
         tBB->cfg.attach(&endBB->cfg, Graph::Edge::FORWARD);
         mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

         setPosition(fBB, true);

         tBB = new BasicBlock(func);
         fBB = new BasicBlock(func);

         bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
         bb->cfg.attach(&tBB->cfg, Graph::Edge::TREE);
         mkCmp(OP_SET, CC_LT, TYPE_U8, pred, TYPE_F32, op2, op1);
         mkFlow(OP_BRA, tBB, CC_P, pred);

         setPosition(fBB, true);
         fBB = new BasicBlock(func);
         bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
         mkFlow(OP_BRA, fBB, CC_ALWAYS, nullptr);

         setPosition(tBB, true);
         mkOp2(OP_SUB, TYPE_S32, res, op1, tmp);
         tBB->cfg.attach(&endBB->cfg, Graph::Edge::FORWARD);
         mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

         setPosition(fBB, true);
         bb->cfg.attach(&endBB->cfg, Graph::Edge::TREE);
         mkOp1(OP_MOV, TYPE_U32, res, op1);
         mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

         setPosition(endBB, true);

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, { res }, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Degrees:
   case OpenCLLIB::Radians:
      {
         spv::Id srcId = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(srcId)->second.value.size(); ++i) {
            auto op1 = getOp(srcId, i);
            DataType dType = type->getElementEnumType(i);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);

            if (dType == TYPE_F64)
               mkOp2(OP_MUL, dType, res, op1, loadImm(getScratch(8), op == OpenCLLIB::Degrees ? 0x404ca5dc1a63c1f8LU : 0x3f91df46a2529d39LU));
            else
               mkOp2(OP_MUL, dType, res, op1, loadImm(getScratch(), op == OpenCLLIB::Degrees ? 0x42652ee1 : 0x3c8efa35));

            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Mix:
      {
         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         spv::Id src2Id = spirv::getOperand<spv::Id>(parsedInstruction, 6u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            auto op3 = getOp(src2Id, i);
            DataType dType = type->getElementEnumType(i);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);

            mkOp2(OP_SUB, dType, res, op2, op1);
            mkOp2(OP_MUL, dType, res, res, op3);
            mkOp2(OP_ADD, dType, res, res, op1);

            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Step:
      {
         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            DataType dType = type->getElementEnumType(i);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);
            auto pred = getScratch(1, FILE_PREDICATE);

            BasicBlock *tBB = new BasicBlock(func);
            BasicBlock *fBB = new BasicBlock(func);
            BasicBlock *endBB = new BasicBlock(func);

            bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
            bb->cfg.attach(&tBB->cfg, Graph::Edge::TREE);
            mkCmp(OP_SET, CC_LT, TYPE_U8, pred, dType, op2, op1);
            mkFlow(OP_BRA, tBB, CC_P, pred);

            setPosition(tBB, true);
            bb->cfg.attach(&endBB->cfg, Graph::Edge::FORWARD);
            if (dType == TYPE_F64)
               mkMov(res, loadImm(getScratch(8), 0x0000000000000000UL), dType);
            else
               mkMov(res, loadImm(getScratch(), 0x00000000), dType);
            mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

            setPosition(fBB, true);
            bb->cfg.attach(&endBB->cfg, Graph::Edge::TREE);
            if (dType == TYPE_F64)
               mkMov(res, loadImm(getScratch(8), 0x3ff0000000000000UL), dType);
            else
               mkMov(res, loadImm(getScratch(), 0x3f800000), dType);
            mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

            setPosition(endBB, true);
            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Smoothstep:
      {
         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         spv::Id src2Id = spirv::getOperand<spv::Id>(parsedInstruction, 6u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            auto op3 = getOp(src2Id, i);
            DataType dType = type->getElementEnumType(i);
            auto tmp0 = getScratch(dType == TYPE_F64 ? 8 : 4);
            auto tmp1 = getScratch(dType == TYPE_F64 ? 8 : 4);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);

            mkOp2(OP_SUB, dType, tmp0, op3, op1);
            mkOp2(OP_SUB, dType, tmp1, op2, op1);
            mkOp2(OP_DIV, dType, tmp0, tmp0, tmp1);
            mkOp1(OP_SAT, dType, tmp0, tmp0);
            if (dType == TYPE_F64) {
               mkOp2(OP_MUL, dType, tmp1, tmp0, loadImm(getScratch(8), 2.0));
               mkOp2(OP_SUB, dType, tmp1, loadImm(getScratch(8), 3.0), tmp1);
            } else {
               mkOp2(OP_MUL, dType, tmp1, tmp0, loadImm(getScratch(), 2.0f));
               mkOp2(OP_SUB, dType, tmp1, loadImm(getScratch(), 3.0f), tmp1);
            }
            mkOp2(OP_MUL, dType, tmp1, tmp1, tmp0);
            mkOp2(OP_MUL, dType, res, tmp1, tmp0);

            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Sign:
      {
         spv::Id srcId = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         std::vector<PValue> values;
         // +1.0 for x > 0
         // -1.0 for x < 0
         // +0.0 for x == inf
         // -0.0 for x == -inf
         //  0.0 for x == NaN
         for (int i = 0; i < spvValues.find(srcId)->second.value.size(); ++i) {
            auto op1 = getOp(srcId, i);
            DataType dType = type->getElementEnumType(i);
            auto res = getScratch(dType == TYPE_F64 ? 8 : 4);
            auto pred = getScratch(1, FILE_PREDICATE);

            BasicBlock *tBB = new BasicBlock(func);
            BasicBlock *fBB = new BasicBlock(func);
            BasicBlock *endBB = new BasicBlock(func);

            bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
            bb->cfg.attach(&tBB->cfg, Graph::Edge::TREE);
            // NaN needs special handling
            if (dType == TYPE_F64) {
               mkOp2(OP_AND, TYPE_U64, res, op1, loadImm(getScratch(8), 0x7fffffffffffffffUL));
               mkCmp(OP_SET, CC_EQ, TYPE_U8, pred, TYPE_U64, res, loadImm(getScratch(8), 0x7ff0000000000000UL));
            } else {
               mkOp2(OP_AND, TYPE_U32, res, op1, loadImm(getScratch(), 0x7fffffff));
               mkCmp(OP_SET, CC_EQ, TYPE_U8, pred, TYPE_U32, res, loadImm(getScratch(), 0x7f800000));
            }
            mkFlow(OP_BRA, tBB, CC_P, pred);

            setPosition(tBB, true);
            bb->cfg.attach(&endBB->cfg, Graph::Edge::FORWARD);
            mkMov(res, op1, dType);
            mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

            setPosition(fBB, true);
            tBB = new BasicBlock(func);
            fBB = new BasicBlock(func);
            bb->cfg.attach(&fBB->cfg, Graph::Edge::TREE);
            bb->cfg.attach(&tBB->cfg, Graph::Edge::TREE);
            if (dType == TYPE_F64)
               mkCmp(OP_SET, CC_GT, TYPE_U8, pred, TYPE_U64, res, loadImm(getScratch(8), 0x7ff0000000000000UL));
            else
               mkCmp(OP_SET, CC_GT, TYPE_U8, pred, TYPE_U32, res, loadImm(getScratch(), 0x7f800000));
            mkFlow(OP_BRA, tBB, CC_P, pred);

            setPosition(tBB, true);
            bb->cfg.attach(&endBB->cfg, Graph::Edge::CROSS);
            if (dType == TYPE_F64)
               loadImm(res, 0.);
            else
               loadImm(res, 0.f);
            mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

            setPosition(fBB, true);
            bb->cfg.attach(&endBB->cfg, Graph::Edge::TREE);
            if (dType == TYPE_F64) {
               mkOp2(OP_AND, TYPE_U64, res, op1, loadImm(getScratch(8), 0x8000000000000000UL));
               mkOp2(OP_OR, TYPE_U64, res, res, loadImm(getScratch(8), 0x3ff0000000000000UL));
            } else {
               mkOp2(OP_AND, TYPE_U32, res, op1, loadImm(getScratch(), 0x80000000));
               mkOp2(OP_OR, TYPE_U32, res, res, loadImm(getScratch(), 0x3f800000));
            }
            mkFlow(OP_BRA, endBB, CC_ALWAYS, nullptr);

            setPosition(endBB, true);
            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::SHadd:
   case OpenCLLIB::UHadd:
      {
         auto op1 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 4u), 0u);
         auto op2 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 5u), 0u);
         auto res = getScratch();
         DataType dType = type->getEnumType(op == OpenCLLIB::SHadd);

         mkOp2(OP_ADD, dType, res, op1, op2);
         mkOp3(OP_MAD, dType, res, res, loadImm(getScratch(), 2), res)->subOp = NV50_IR_SUBOP_MUL_HIGH;
         mkOp2(OP_SHR, dType, res, res, loadImm(getScratch(), 1));

         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, { res }, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Rotate:
      {
         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            unsigned int dTypeSize = typeSizeof(type->getElementEnumType(i));
            DataType oriDType = typeOfSize(dTypeSize), dType;
            auto res = getScratch(std::max(4u, dTypeSize));

            if (dTypeSize < 8)
               dType = TYPE_U32;
            else {
               _debug_printf("OpenCLLIB::Rotate is broken for TYPE_U64\n");
               dType = TYPE_U64;
            }

            auto tmp0 = getScratch(typeSizeof(dType));
            auto tmp1 = getScratch(typeSizeof(dType));

            if (dTypeSize > 4)
               mkOp2(OP_AND, dType, tmp0, op2, loadImm(getScratch(typeSizeof(dType)), (uint64_t)dTypeSize * 8 - 1));
            else
               mkOp2(OP_AND, dType, tmp0, op2, loadImm(getScratch(typeSizeof(dType)), dTypeSize * 8 - 1));
            mkOp2(OP_SHL, dType, tmp1, op1, tmp0);

            if (dTypeSize > 4)
               mkOp2(OP_SUB, dType, tmp0, loadImm(getScratch(typeSizeof(dType)), (uint64_t)dTypeSize * 8), tmp0);
            else
               mkOp2(OP_SUB, dType, tmp0, loadImm(getScratch(typeSizeof(dType)), dTypeSize * 8), tmp0);
            mkOp2(OP_SHR, dType, tmp0, op1, tmp0);
            mkOp2(OP_OR, dType, tmp0, tmp0, tmp1);
            if (dTypeSize > 4)
               mkOp2(OP_AND, dType, res, tmp0, loadImm(getScratch(typeSizeof(dType)), (uint64_t)-1l));
            else
               mkOp2(OP_AND, dType, res, tmp0, loadImm(getScratch(typeSizeof(dType)), -1));

            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::SMad24:
   case OpenCLLIB::UMad24:
      {
         auto op1 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 4u), 0u);
         auto op2 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 5u), 0u);
         auto op3 = getOp(spirv::getOperand<spv::Id>(parsedInstruction, 6u), 0u);
         auto res = getScratch();
         mkMAD24(res, type->getEnumType(op == OpenCLLIB::SMad24), op1, op2, op3);
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, { res }, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Vloadn:
      {
         std::vector<PValue> values;
         for (int i = 0; i < type->getElementsNb(); ++i) {
            DataType dType = typeOfSize(typeSizeof(type->getElementEnumType(i)));
            auto res = getScratch(std::max(4u, typeSizeof(dType)));
            loadImm(res, 0);
            values.push_back(res);
         }

         _debug_printf("Unsupported OpenCLLIB opcode %u\n", op);
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Vstoren:
      {
         _debug_printf("Unsupported OpenCLLIB opcode %u\n", op);
         return SPV_SUCCESS;
      }
      break;
   case OpenCLLIB::Bitselect:
      {
         spv::Id src0Id = spirv::getOperand<spv::Id>(parsedInstruction, 4u);
         spv::Id src1Id = spirv::getOperand<spv::Id>(parsedInstruction, 5u);
         spv::Id src2Id = spirv::getOperand<spv::Id>(parsedInstruction, 6u);
         std::vector<PValue> values;
         for (int i = 0; i < spvValues.find(src0Id)->second.value.size(); ++i) {
            auto op1 = getOp(src0Id, i);
            auto op2 = getOp(src1Id, i);
            auto op3 = getOp(src2Id, i);
            unsigned int dTypeSize = typeSizeof(type->getElementEnumType(i));
            DataType oriDType = typeOfSize(dTypeSize), dType;
            auto res = getScratch(std::max(4u, dTypeSize));

            if (dTypeSize < 8)
               dType = TYPE_U32;
            else
               dType = TYPE_U64;

            auto tmp = getScratch(typeSizeof(dType));

            mkOp1(OP_NOT, dType, tmp, op3);
            mkOp2(OP_AND, dType, res, tmp, op1);
            mkOp2(OP_AND, dType, tmp, op2, op3);
            mkOp2(OP_OR, dType, res, res, tmp);

            values.push_back(res);
         }
         spvValues.emplace(resId, SpirVValue{ SpirvFile::TEMPORARY, type, values, type->getPaddings() });
         return SPV_SUCCESS;
      }
      break;
   }

   _debug_printf("Unsupported OpenCLLIB opcode %u\n", op);
   return SPV_UNSUPPORTED;
}

int
Converter::getSubOp(spv::Op opcode) const
{
   switch (opcode) {
   case spv::Op::OpAtomicIIncrement: return NV50_IR_SUBOP_ATOM_INC;
   case spv::Op::OpAtomicIDecrement: return NV50_IR_SUBOP_ATOM_ADD;
   case spv::Op::OpAtomicIAdd: return NV50_IR_SUBOP_ATOM_ADD;
   case spv::Op::OpAtomicISub: return NV50_IR_SUBOP_ATOM_ADD;
   case spv::Op::OpAtomicSMin: /* fallthrough */
   case spv::Op::OpAtomicUMin: return NV50_IR_SUBOP_ATOM_MIN;
   case spv::Op::OpAtomicSMax: /* fallthrough */
   case spv::Op::OpAtomicAnd: return NV50_IR_SUBOP_ATOM_AND;
   case spv::Op::OpAtomicOr: return NV50_IR_SUBOP_ATOM_OR;
   case spv::Op::OpAtomicXor: return NV50_IR_SUBOP_ATOM_XOR;
   case spv::Op::OpAtomicCompareExchange: return NV50_IR_SUBOP_ATOM_CAS;
   case spv::Op::OpAtomicExchange: return NV50_IR_SUBOP_ATOM_EXCH;
   default: assert(false); return 0;
   }
}

TexTarget
Converter::getTexTarget(TypeImage const* type)
{
   switch (type->dim) {
   case spv::Dim::Dim1D:
      if (type->arrayed && type->depth == 1)
         return TexTarget::TEX_TARGET_1D_ARRAY_SHADOW;
      else if (type->arrayed)
         return TexTarget::TEX_TARGET_1D_ARRAY;
      else if (type->depth == 1)
         return TexTarget::TEX_TARGET_1D_SHADOW;
      else
         return TexTarget::TEX_TARGET_1D;
   case spv::Dim::Dim2D:
      if (type->arrayed && type->depth == 1)
         return TexTarget::TEX_TARGET_2D_ARRAY_SHADOW;
      else if (type->arrayed && type->ms)
         return TexTarget::TEX_TARGET_2D_MS_ARRAY;
      else if (type->arrayed)
         return TexTarget::TEX_TARGET_2D_ARRAY;
      else if (type->depth == 1)
         return TexTarget::TEX_TARGET_2D_SHADOW;
      else if (type->ms)
         return TexTarget::TEX_TARGET_2D_MS;
      else
         return TexTarget::TEX_TARGET_2D;
   case spv::Dim::Dim3D:
      return TexTarget::TEX_TARGET_3D;
   case spv::Dim::Rect:
      if (type->depth == 1)
         return TexTarget::TEX_TARGET_RECT_SHADOW;
      else
         return TexTarget::TEX_TARGET_RECT;
   case spv::Dim::Buffer:
      return TexTarget::TEX_TARGET_BUFFER;
   case spv::Dim::Cube:
      if (type->arrayed && type->depth == 1)
         return TexTarget::TEX_TARGET_CUBE_ARRAY_SHADOW;
      if (type->arrayed)
         return TexTarget::TEX_TARGET_CUBE_ARRAY;
      if (type->depth == 1)
         return TexTarget::TEX_TARGET_CUBE_SHADOW;
      else
         return TexTarget::TEX_TARGET_CUBE;
   case spv::Dim::SubpassData:
      assert(false && "Unsupported Dim::SubpassData");
      return TexTarget::TEX_TARGET_1D;
   }
   return TexTarget::TEX_TARGET_UNKNOWN;
}

TexInstruction::ImgFormatDesc const*
Converter::getImageFormat(spv::ImageFormat format)
{
   ImgFormat imgFormat = ImgFormat::FMT_NONE;

#define NV50_IR_TRANS_IMG_FORMAT(a, b) case a: imgFormat = FMT_##b; break;

   switch (format) {
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Unknown, NONE)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba32f, RGBA32F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba16f, RGBA16F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R32f, R32F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba8, RGBA8)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba8Snorm, RGBA8_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg32f, RG32F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg16f, RG16F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R11fG11fB10f, R11G11B10F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R16f, R16F)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba16, RGBA16)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgb10A2, RGB10A2)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg16, RG16)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg8, RG8)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R16, R16)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R8, R8)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba16Snorm, RGBA16_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg16Snorm, RG16_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg8Snorm, RG8_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R16Snorm, R16_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R8Snorm, R8_SNORM)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba32i, RGBA32I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba16i, RGBA16I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba8i, RGBA8I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R32i, R32I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg32i, RG32I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg16i, RG16I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg8i, RG8I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R16i, R16I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R8i, R8I)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba32ui, RGBA32UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba16ui, RGBA16UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgba8ui, RGBA8UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R32ui, R32UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rgb10a2ui, RGB10A2UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg32ui, RG32UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg16ui, RG16UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::Rg8ui, RG8UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R16ui, R16UI)
      NV50_IR_TRANS_IMG_FORMAT(spv::ImageFormat::R8ui, R8UI)
   }

#undef NV50_IR_TRANS_IMG_FORMAT

   return &nv50_ir::TexInstruction::formatTable[imgFormat];
}

} // unnamed namespace

namespace nv50_ir {

bool
Program::makeFromSPIRV(struct nv50_ir_prog_info *info)
{
   Converter builder(this, info);
   return builder.run();
}

} // namespace nv50_ir
