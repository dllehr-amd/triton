#include "TritonGPUToLLVM.h"
#include "DotOpHelpers.h"
#include "Utility.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::LLVM::getElementsFromStruct;
using ::mlir::LLVM::getSharedMemoryObjectFromStruct;
using ::mlir::LLVM::getStructFromElements;
using ::mlir::triton::gpu::getElemsPerThread;
using ::mlir::triton::gpu::SharedEncodingAttr;

struct ReturnOpConversion : public ConvertOpToLLVMPattern<::mlir::ReturnOp> {
  using ConvertOpToLLVMPattern<ReturnOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    unsigned numArguments = op.getNumOperands();

    // Currently, Triton kernel function always return nothing.
    // TODO(Superjomn) add support for non-inline device function
    if (numArguments > 0) {
      return rewriter.notifyMatchFailure(
          op, "Only kernel function with nothing returned is supported.");
    }

    rewriter.replaceOpWithNewOp<LLVM::ReturnOp>(op, TypeRange(), ValueRange(),
                                                op->getAttrs());
    return success();
  }
};

struct BroadcastOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::BroadcastOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::BroadcastOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::BroadcastOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Following the order of indices in the legacy code, a broadcast of:
    //   [s(0), s(1) ... s(k-1),    1, s(k+1), s(k+2) ... s(n-1)]
    // =>
    //   [s(0), s(1) ... s(k-1), s(k), s(k+1), s(k+2) ... s(n-1)]
    //
    // logically maps to a broadcast within a thread's scope:
    //   [cta(0)..cta(k-1),     1,cta(k+1)..cta(n-1),spt(0)..spt(k-1),
    //   1,spt(k+1)..spt(n-1)]
    // =>
    //   [cta(0)..cta(k-1),cta(k),cta(k+1)..cta(n-1),spt(0)..spt(k-1),spt(k),spt(k+1)..spt(n-1)]
    //
    // regardless of the order of the layout
    //
    Location loc = op->getLoc();
    Value src = adaptor.src();
    Value result = op.result();
    auto srcTy = op.src().getType().cast<RankedTensorType>();
    auto resultTy = result.getType().cast<RankedTensorType>();
    auto srcLayout = srcTy.getEncoding();
    auto resultLayout = resultTy.getEncoding();
    auto srcShape = srcTy.getShape();
    auto resultShape = resultTy.getShape();
    unsigned rank = srcTy.getRank();

    assert(rank == resultTy.getRank());
    auto order = triton::gpu::getOrder(srcLayout);
    auto srcOffsets = emitOffsetForLayout(srcLayout, srcShape);
    auto resultOffsets = emitOffsetForLayout(resultLayout, resultShape);
    SmallVector<Value> srcVals = getElementsFromStruct(loc, src, rewriter);
    DenseMap<SmallVector<unsigned>, Value, SmallVectorKeyInfo> srcValues;
    for (size_t i = 0; i < srcOffsets.size(); i++) {
      srcValues[srcOffsets[i]] = srcVals[i];
    }
    SmallVector<Value> resultVals;
    for (size_t i = 0; i < resultOffsets.size(); i++) {
      auto offset = resultOffsets[i];
      for (size_t j = 0; j < srcShape.size(); j++)
        if (srcShape[j] == 1)
          offset[j] = 0;
      resultVals.push_back(srcValues.lookup(offset));
    }
    auto llvmStructTy = getTypeConverter()->convertType(resultTy);
    Value resultStruct =
        getStructFromElements(loc, resultVals, rewriter, llvmStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

struct PrintfOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::PrintfOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::PrintfOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::PrintfOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();
    SmallVector<Value, 16> operands;
    for (auto operand : adaptor.getOperands()) {
      auto sub_operands = getElementsFromStruct(loc, operand, rewriter);
      for (auto elem : sub_operands) {
        operands.push_back(elem);
      }
    }
    std::string formatStr;
    llvm::raw_string_ostream os(formatStr);
    os << op.prefix();
    if (!operands.empty()) {
      os << getFormatSubstr(operands[0]);
    }

    for (size_t i = 1; i < operands.size(); ++i) {
      os << ", " << getFormatSubstr(operands[i]);
    }
    llPrintf(formatStr, operands, rewriter);
    rewriter.eraseOp(op);
    return success();
  }

  std::string getFormatSubstr(Value value) const {
    Type type = value.getType();
    if (type.isa<LLVM::LLVMPointerType>()) {
      return "%p";
    } else if (type.isBF16() || type.isF16() || type.isF32() || type.isF64()) {
      return "%f";
    } else if (type.isSignedInteger()) {
      return "%i";
    } else if (type.isUnsignedInteger() || type.isSignlessInteger()) {
      return "%u";
    }
    assert(false && "not supported type");
    return "";
  }

  // declare vprintf(i8*, i8*) as external function
  static LLVM::LLVMFuncOp
  getVprintfDeclaration(ConversionPatternRewriter &rewriter) {
    auto moduleOp =
        rewriter.getBlock()->getParent()->getParentOfType<ModuleOp>();
    StringRef funcName("vprintf");
    Operation *funcOp = moduleOp.lookupSymbol(funcName);
    if (funcOp)
      return cast<LLVM::LLVMFuncOp>(*funcOp);

    auto *context = rewriter.getContext();

    SmallVector<Type> argsType{ptr_ty(IntegerType::get(context, 8)),
                               ptr_ty(IntegerType::get(context, 8))};
    auto funcType = LLVM::LLVMFunctionType::get(i32_ty, argsType);

    ConversionPatternRewriter::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointToStart(moduleOp.getBody());

    return rewriter.create<LLVM::LLVMFuncOp>(UnknownLoc::get(context), funcName,
                                             funcType);
  }

  // extend integer to int32, extend float to float64
  // this comes from vprintf alignment requirements.
  static std::pair<Type, Value>
  promoteValue(ConversionPatternRewriter &rewriter, Value value) {
    auto *context = rewriter.getContext();
    auto type = value.getType();
    Value newOp = value;
    Type newType = type;

    bool bUnsigned = type.isUnsignedInteger();
    if (type.isIntOrIndex() && type.getIntOrFloatBitWidth() < 32) {
      if (bUnsigned) {
        newType = ui32_ty;
        newOp = rewriter.create<LLVM::ZExtOp>(UnknownLoc::get(context), newType,
                                              value);
      } else {
        newType = i32_ty;
        newOp = rewriter.create<LLVM::SExtOp>(UnknownLoc::get(context), newType,
                                              value);
      }
    } else if (type.isBF16() || type.isF16() || type.isF32()) {
      newType = f64_ty;
      newOp = rewriter.create<LLVM::FPExtOp>(UnknownLoc::get(context), newType,
                                             value);
    }

    return {newType, newOp};
  }

  static void llPrintf(StringRef msg, ValueRange args,
                       ConversionPatternRewriter &rewriter) {
    static const char formatStringPrefix[] = "printfFormat_";
    assert(!msg.empty() && "printf with empty string not support");
    Type int8Ptr = ptr_ty(i8_ty);

    auto *context = rewriter.getContext();
    auto moduleOp =
        rewriter.getBlock()->getParent()->getParentOfType<ModuleOp>();
    auto funcOp = getVprintfDeclaration(rewriter);

    Value one = rewriter.create<LLVM::ConstantOp>(
        UnknownLoc::get(context), i32_ty, rewriter.getI32IntegerAttr(1));
    Value zero = rewriter.create<LLVM::ConstantOp>(
        UnknownLoc::get(context), i32_ty, rewriter.getI32IntegerAttr(0));

    unsigned stringNumber = 0;
    SmallString<16> stringConstName;
    do {
      stringConstName.clear();
      (formatStringPrefix + Twine(stringNumber++)).toStringRef(stringConstName);
    } while (moduleOp.lookupSymbol(stringConstName));

    llvm::SmallString<64> formatString(msg);
    formatString.push_back('\n');
    formatString.push_back('\0');
    size_t formatStringSize = formatString.size_in_bytes();
    auto globalType = LLVM::LLVMArrayType::get(i8_ty, formatStringSize);

    LLVM::GlobalOp global;
    {
      ConversionPatternRewriter::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToStart(moduleOp.getBody());
      global = rewriter.create<LLVM::GlobalOp>(
          UnknownLoc::get(context), globalType,
          /*isConstant=*/true, LLVM::Linkage::Internal, stringConstName,
          rewriter.getStringAttr(formatString));
    }

    Value globalPtr =
        rewriter.create<LLVM::AddressOfOp>(UnknownLoc::get(context), global);
    Value stringStart = rewriter.create<LLVM::GEPOp>(
        UnknownLoc::get(context), int8Ptr, globalPtr,
        SmallVector<Value>({zero, zero}));

    Value bufferPtr =
        rewriter.create<LLVM::NullOp>(UnknownLoc::get(context), int8Ptr);

    SmallVector<Value, 16> newArgs;
    if (args.size() >= 1) {
      SmallVector<Type> argTypes;
      for (auto arg : args) {
        Type newType;
        Value newArg;
        std::tie(newType, newArg) = promoteValue(rewriter, arg);
        argTypes.push_back(newType);
        newArgs.push_back(newArg);
      }

      Type structTy = LLVM::LLVMStructType::getLiteral(context, argTypes);
      auto allocated = rewriter.create<LLVM::AllocaOp>(UnknownLoc::get(context),
                                                       ptr_ty(structTy), one,
                                                       /*alignment=*/0);

      for (const auto &entry : llvm::enumerate(newArgs)) {
        auto index = rewriter.create<LLVM::ConstantOp>(
            UnknownLoc::get(context), i32_ty,
            rewriter.getI32IntegerAttr(entry.index()));
        auto fieldPtr = rewriter.create<LLVM::GEPOp>(
            UnknownLoc::get(context), ptr_ty(argTypes[entry.index()]),
            allocated, ArrayRef<Value>{zero, index});
        rewriter.create<LLVM::StoreOp>(UnknownLoc::get(context), entry.value(),
                                       fieldPtr);
      }
      bufferPtr = rewriter.create<LLVM::BitcastOp>(UnknownLoc::get(context),
                                                   int8Ptr, allocated);
    }

    SmallVector<Value> operands{stringStart, bufferPtr};
    rewriter.create<LLVM::CallOp>(UnknownLoc::get(context), funcOp, operands);
  }
};

struct MakeRangeOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::MakeRangeOp> {

  MakeRangeOpConversion(
      LLVMTypeConverter &converter,
      ConvertTritonGPUOpToLLVMPatternBase::IndexCacheInfo &indexCacheInfo,
      PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::MakeRangeOp>(
            converter, /*Allocation*/ nullptr, Value{}, indexCacheInfo,
            benefit) {}

  LogicalResult
  matchAndRewrite(triton::MakeRangeOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto rankedTy = op.result().getType().dyn_cast<RankedTensorType>();
    auto shape = rankedTy.getShape();
    auto layout = rankedTy.getEncoding();

    auto elemTy = rankedTy.getElementType();
    assert(elemTy.isInteger(32));
    Value start = createIndexAttrConstant(rewriter, loc, elemTy, op.start());
    auto idxs = emitIndices(loc, rewriter, layout, shape);
    unsigned elems = idxs.size();
    SmallVector<Value> retVals(elems);
    // TODO: slice layout has more elements than expected.
    // Unexpected behavior for make range, but generally OK when followed by
    // expand dims + broadcast. very weird behavior otherwise potentially.
    for (const auto multiDim : llvm::enumerate(idxs)) {
      assert(multiDim.value().size() == 1);
      retVals[multiDim.index()] = add(multiDim.value()[0], start);
    }
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    Value result = getStructFromElements(loc, retVals, rewriter, structTy);
    rewriter.replaceOp(op, result);
    return success();
  }
};

struct GetProgramIdOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::GetProgramIdOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::GetProgramIdOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::GetProgramIdOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    assert(op.axis() < 3);

    Value blockId = rewriter.create<::mlir::gpu::BlockIdOp>(
        loc, rewriter.getIndexType(), dims[op.axis()]);
    auto llvmIndexTy = getTypeConverter()->getIndexType();
    rewriter.replaceOpWithNewOp<UnrealizedConversionCastOp>(
        op, TypeRange{llvmIndexTy}, ValueRange{blockId});
    return success();
  }

  static constexpr mlir::gpu::Dimension dims[] = {mlir::gpu::Dimension::x,
                                                  mlir::gpu::Dimension::y,
                                                  mlir::gpu::Dimension::z};
};

struct GetNumProgramsOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::GetNumProgramsOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::GetNumProgramsOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::GetNumProgramsOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    assert(op.axis() < 3);

    Value blockId = rewriter.create<::mlir::gpu::GridDimOp>(
        loc, rewriter.getIndexType(), dims[op.axis()]);
    auto llvmIndexTy = getTypeConverter()->getIndexType();
    rewriter.replaceOpWithNewOp<UnrealizedConversionCastOp>(
        op, TypeRange{llvmIndexTy}, ValueRange{blockId});
    return success();
  }

  static constexpr mlir::gpu::Dimension dims[] = {mlir::gpu::Dimension::x,
                                                  mlir::gpu::Dimension::y,
                                                  mlir::gpu::Dimension::z};
};

struct AddPtrOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::AddPtrOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::AddPtrOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::AddPtrOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto resultTy = op.getType();
    auto resultTensorTy = resultTy.dyn_cast<RankedTensorType>();
    if (resultTensorTy) {
      unsigned elems = getElemsPerThread(resultTy);
      Type elemTy =
          getTypeConverter()->convertType(resultTensorTy.getElementType());
      SmallVector<Type> types(elems, elemTy);
      Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
      auto ptrs = getElementsFromStruct(loc, adaptor.ptr(), rewriter);
      auto offsets = getElementsFromStruct(loc, adaptor.offset(), rewriter);
      SmallVector<Value> resultVals(elems);
      for (unsigned i = 0; i < elems; ++i) {
        resultVals[i] = gep(elemTy, ptrs[i], offsets[i]);
      }
      Value view = getStructFromElements(loc, resultVals, rewriter, structTy);
      rewriter.replaceOp(op, view);
    } else {
      assert(resultTy.isa<triton::PointerType>());
      Type llResultTy = getTypeConverter()->convertType(resultTy);
      Value result = gep(llResultTy, adaptor.ptr(), adaptor.offset());
      rewriter.replaceOp(op, result);
    }
    return success();
  }
};

struct AllocTensorOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::gpu::AllocTensorOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::gpu::AllocTensorOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::gpu::AllocTensorOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value smemBase = getSharedMemoryBase(loc, rewriter, op.getResult());
    auto resultTy = op.getType().dyn_cast<RankedTensorType>();
    auto llvmElemTy =
        getTypeConverter()->convertType(resultTy.getElementType());
    auto elemPtrTy = ptr_ty(llvmElemTy, 3);
    smemBase = bitcast(smemBase, elemPtrTy);
    auto order = resultTy.getEncoding().cast<SharedEncodingAttr>().getOrder();
    // Workaround for 3D tensors
    // TODO: we need to modify the pipeline pass to give a proper shared
    // encoding to 3D tensors
    SmallVector<unsigned> newOrder;
    if (resultTy.getShape().size() == 3)
      newOrder = {1 + order[0], 1 + order[1], 0};
    else
      newOrder = SmallVector<unsigned>(order.begin(), order.end());

    auto smemObj = SharedMemoryObject(smemBase, resultTy.getShape(), newOrder,
                                      loc, rewriter);
    auto retVal = getStructFromSharedMemoryObject(loc, smemObj, rewriter);
    rewriter.replaceOp(op, retVal);
    return success();
  }
};

struct ExtractSliceOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<tensor::ExtractSliceOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      tensor::ExtractSliceOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(tensor::ExtractSliceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // %dst = extract_slice %src[%offsets]
    Location loc = op->getLoc();
    auto srcTy = op.source().getType().dyn_cast<RankedTensorType>();
    auto srcLayout = srcTy.getEncoding().dyn_cast<SharedEncodingAttr>();
    assert(srcLayout && "Unexpected resultLayout in ExtractSliceOpConversion");
    assert(op.hasUnitStride() &&
           "Only unit stride supported by ExtractSliceOpConversion");

    // newBase = base + offset
    // Triton supports either static and dynamic offsets
    auto smemObj =
        getSharedMemoryObjectFromStruct(loc, adaptor.source(), rewriter);
    SmallVector<Value, 4> opOffsetVals;
    SmallVector<Value, 4> offsetVals;
    auto mixedOffsets = op.getMixedOffsets();
    for (auto i = 0; i < mixedOffsets.size(); ++i) {
      if (op.isDynamicOffset(i))
        opOffsetVals.emplace_back(adaptor.offsets()[i]);
      else
        opOffsetVals.emplace_back(i32_val(op.getStaticOffset(i)));
      offsetVals.emplace_back(add(smemObj.offsets[i], opOffsetVals[i]));
    }
    // Compute the offset based on the original strides of the shared memory
    // object
    auto offset = dot(rewriter, loc, opOffsetVals, smemObj.strides);
    // newShape = rank_reduce(shape)
    // Triton only supports static tensor sizes
    SmallVector<Value, 4> strideVals;
    for (auto i = 0; i < op.static_sizes().size(); ++i) {
      if (op.getStaticSize(i) == 1) {
        offsetVals.erase(offsetVals.begin() + i);
      } else {
        strideVals.emplace_back(smemObj.strides[i]);
      }
    }

    auto llvmElemTy = getTypeConverter()->convertType(srcTy.getElementType());
    auto elemPtrTy = ptr_ty(llvmElemTy, 3);
    auto resTy = op.getType().dyn_cast<RankedTensorType>();
    smemObj = SharedMemoryObject(gep(elemPtrTy, smemObj.base, offset),
                                 strideVals, offsetVals);
    auto retVal = getStructFromSharedMemoryObject(loc, smemObj, rewriter);
    rewriter.replaceOp(op, retVal);
    return success();
  }
};

struct AsyncWaitOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::gpu::AsyncWaitOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::gpu::AsyncWaitOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::gpu::AsyncWaitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    PTXBuilder ptxBuilder;
    auto &asyncWaitOp = *ptxBuilder.create<>("cp.async.wait_group");
    auto num = op->getAttrOfType<IntegerAttr>("num").getInt();
    asyncWaitOp(ptxBuilder.newConstantOperand(num));

    auto ctx = op.getContext();
    auto loc = op.getLoc();
    auto voidTy = void_ty(ctx);
    ptxBuilder.launch(rewriter, loc, voidTy);

    // Safe to remove the op since it doesn't have any return value.
    rewriter.eraseOp(op);
    return success();
  }
};

void populateTritonGPUToLLVMPatterns(
    mlir::LLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    int numWarps, AxisInfoAnalysis &axisInfoAnalysis,
    const Allocation *allocation, Value smem,
    ConvertTritonGPUOpToLLVMPatternBase::IndexCacheInfo &indexCacheInfo,
    PatternBenefit benefit) {
  patterns.add<AddPtrOpConversion>(typeConverter, benefit);
  patterns.add<AllocTensorOpConversion>(typeConverter, allocation, smem,
                                        benefit);
  patterns.add<AsyncWaitOpConversion>(typeConverter, benefit);
  patterns.add<BroadcastOpConversion>(typeConverter, benefit);

  patterns.add<ExtractSliceOpConversion>(typeConverter, allocation, smem,
                                         benefit);
  patterns.add<GetProgramIdOpConversion>(typeConverter, benefit);
  patterns.add<GetNumProgramsOpConversion>(typeConverter, benefit);
  patterns.add<MakeRangeOpConversion>(typeConverter, indexCacheInfo, benefit);
  patterns.add<ReturnOpConversion>(typeConverter, benefit);
  patterns.add<PrintfOpConversion>(typeConverter, benefit);
}
