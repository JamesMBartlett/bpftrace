#include "codegen_llvm.h"
#include "arch/arch.h"
#include "ast.h"
#include "ast/async_event_types.h"
#include "bpforc.h"
#include "codegen_helper.h"
#include "log.h"
#include "parser.tab.hh"
#include "tracepoint_format_parser.h"
#include "types.h"
#include "usdt.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <fstream>

#include <llvm/Support/TargetRegistry.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm-c/Transforms/IPO.h>

namespace bpftrace {
namespace ast {

CodegenLLVM::CodegenLLVM(Node *root, BPFtrace &bpftrace)
    : root_(root),
      module_(std::make_unique<Module>("bpftrace", context_)),
      b_(context_, *module_.get(), bpftrace),
      layout_(module_.get()),
      bpftrace_(bpftrace)
{
  LLVMInitializeBPFTargetInfo();
  LLVMInitializeBPFTarget();
  LLVMInitializeBPFTargetMC();
  LLVMInitializeBPFAsmPrinter();

  std::string targetTriple = "bpf-pc-linux";
  module_->setTargetTriple(targetTriple);

  std::string error;
  const Target *target = TargetRegistry::lookupTarget(targetTriple, error);
  if (!target)
    throw std::runtime_error("Could not create LLVM target " + error);

  TargetOptions opt;
  auto RM = Reloc::Model();
  TM_ = target->createTargetMachine(targetTriple, "generic", "", opt, RM);
  module_->setDataLayout(TM_->createDataLayout());
  layout_ = DataLayout(module_.get());
  orc_ = std::make_unique<BpfOrc>(TM_);
}

void CodegenLLVM::visit(Integer &integer)
{
  expr_ = b_.getInt64(integer.n);
}

void CodegenLLVM::visit(PositionalParameter &param)
{
  switch (param.ptype)
  {
    case PositionalParameterType::positional:
      {
        std::string pstr = bpftrace_.get_param(param.n, param.is_in_str);
        if (is_numeric(pstr))
        {
          expr_ = b_.getInt64(std::stoll(pstr, nullptr, 0));
        }
        else
        {
          Constant *const_str = ConstantDataArray::getString(module_->getContext(), pstr, true);
          AllocaInst *buf = b_.CreateAllocaBPF(ArrayType::get(b_.getInt8Ty(), pstr.length() + 1), "str");
          b_.CREATE_MEMSET(buf, b_.getInt8(0), pstr.length() + 1, 1);
          b_.CreateStore(const_str, buf);
          expr_ = buf;
          expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
        }
      }
      break;
    case PositionalParameterType::count:
      expr_ = b_.getInt64(bpftrace_.num_params());
      break;
  }
}

void CodegenLLVM::visit(String &string)
{
  string.str.resize(string.type.size-1);
  Constant *const_str = ConstantDataArray::getString(module_->getContext(), string.str, true);
  AllocaInst *buf = b_.CreateAllocaBPF(string.type, "str");
  b_.CreateStore(const_str, buf);
  expr_ = buf;
  expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
}

// NB: we do not resolve identifiers that are structs. That is because in
// bpftrace you cannot really instantiate a struct.
void CodegenLLVM::visit(Identifier &identifier)
{
  if (bpftrace_.enums_.count(identifier.ident) != 0)
  {
    expr_ = b_.getInt64(bpftrace_.enums_[identifier.ident]);
  }
  else
  {
    LOG(FATAL) << "unknown identifier \"" << identifier.ident << "\"";
  }
}

void CodegenLLVM::visit(Builtin &builtin)
{
  if (builtin.ident == "nsecs")
  {
    expr_ = b_.CreateGetNs(bpftrace_.feature_.has_helper_ktime_get_boot_ns());
  }
  else if (builtin.ident == "elapsed")
  {
    AllocaInst *key = b_.CreateAllocaBPF(b_.getInt64Ty(), "elapsed_key");
    b_.CreateStore(b_.getInt64(0), key);

    auto *map = bpftrace_.maps[MapManager::Type::Elapsed].value();
    auto type = CreateUInt64();
    auto start = b_.CreateMapLookupElem(
        ctx_, map->mapfd_, key, type, builtin.loc);
    expr_ = b_.CreateGetNs(bpftrace_.feature_.has_helper_ktime_get_boot_ns());
    expr_ = b_.CreateSub(expr_, start);
    // start won't be on stack, no need to LifeTimeEnd it
    b_.CreateLifetimeEnd(key);
  }
  else if (builtin.ident == "kstack" || builtin.ident == "ustack")
  {
    Value *stackid = b_.CreateGetStackId(
        ctx_, builtin.ident == "ustack", builtin.type.stack_type, builtin.loc);
    // Kernel stacks should not be differentiated by tid, since the kernel
    // address space is the same between pids (and when aggregating you *want*
    // to be able to correlate between pids in most cases). User-space stacks
    // are special because of ASLR and so we do usym()-style packing.
    if (builtin.ident == "ustack")
    {
      // pack uint64_t with: (uint32_t)stack_id, (uint32_t)pid
      Value *pidhigh = b_.CreateShl(b_.CreateGetPidTgid(), 32);
      stackid = b_.CreateOr(stackid, pidhigh);
    }
    expr_ = stackid;
  }
  else if (builtin.ident == "pid" || builtin.ident == "tid")
  {
    Value *pidtgid = b_.CreateGetPidTgid();
    if (builtin.ident == "pid")
    {
      expr_ = b_.CreateLShr(pidtgid, 32);
    }
    else if (builtin.ident == "tid")
    {
      expr_ = b_.CreateAnd(pidtgid, 0xffffffff);
    }
  }
  else if (builtin.ident == "cgroup")
  {
    expr_ = b_.CreateGetCurrentCgroupId();
  }
  else if (builtin.ident == "uid" || builtin.ident == "gid" || builtin.ident == "username")
  {
    Value *uidgid = b_.CreateGetUidGid();
    if (builtin.ident == "uid"  || builtin.ident == "username")
    {
      expr_ = b_.CreateAnd(uidgid, 0xffffffff);
    }
    else if (builtin.ident == "gid")
    {
      expr_ = b_.CreateLShr(uidgid, 32);
    }
  }
  else if (builtin.ident == "cpu")
  {
    expr_ = b_.CreateGetCpuId();
  }
  else if (builtin.ident == "curtask")
  {
    expr_ = b_.CreateGetCurrentTask();
  }
  else if (builtin.ident == "rand")
  {
    expr_ = b_.CreateGetRandom();
  }
  else if (builtin.ident == "comm")
  {
    AllocaInst *buf = b_.CreateAllocaBPF(builtin.type, "comm");
    // initializing memory needed for older kernels:
    b_.CREATE_MEMSET(buf, b_.getInt8(0), builtin.type.size, 1);
    b_.CreateGetCurrentComm(ctx_, buf, builtin.type.size, builtin.loc);
    expr_ = buf;
    expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
  }
  else if ((!builtin.ident.compare(0, 3, "arg") && builtin.ident.size() == 4 &&
      builtin.ident.at(3) >= '0' && builtin.ident.at(3) <= '9') ||
      builtin.ident == "retval" ||
      builtin.ident == "func")
  {
    if (builtin.type.is_kfarg)
    {
      expr_ = b_.CreatKFuncArg(ctx_, builtin.type, builtin.ident);
      return;
    }

    int offset;
    if (builtin.ident == "retval")
      offset = arch::ret_offset();
    else if (builtin.ident == "func")
      offset = arch::pc_offset();
    else // argX
    {
      int arg_num = atoi(builtin.ident.substr(3).c_str());
      if (probetype(current_attach_point_->provider) == ProbeType::usdt) {
        expr_ = b_.CreateUSDTReadArgument(ctx_,
                                          current_attach_point_,
                                          current_usdt_location_index_,
                                          arg_num,
                                          builtin,
                                          bpftrace_.pid(),
                                          AddrSpace::none,
                                          builtin.loc);
        return;
      }
      offset = arch::arg_offset(arg_num);
    }

    Value *ctx = b_.CreatePointerCast(ctx_, b_.getInt64Ty()->getPointerTo());
    // LLVM optimization is possible to transform `(uint64*)ctx` into
    // `(uint8*)ctx`, but sometimes this causes invalid context access.
    // Mark every context acess to supporess any LLVM optimization.
    expr_ = b_.CreateLoad(b_.getInt64Ty(),
                          b_.CreateGEP(ctx, b_.getInt64(offset)),
                          builtin.ident);
    // LLVM 7.0 <= does not have CreateLoad(*Ty, *Ptr, isVolatile, Name),
    // so call setVolatile() manually
    dyn_cast<LoadInst>(expr_)->setVolatile(true);

    if (builtin.type.IsUsymTy())
    {
      expr_ = b_.CreateUSym(expr_);
      Value *expr = expr_;
      expr_deleter_ = [this, expr]() { b_.CreateLifetimeEnd(expr); };
    }
  }
  else if (!builtin.ident.compare(0, 4, "sarg") && builtin.ident.size() == 5 &&
      builtin.ident.at(4) >= '0' && builtin.ident.at(4) <= '9')
  {
    int sp_offset = arch::sp_offset();
    if (sp_offset == -1)
    {
      LOG(FATAL) << "negative offset for stack pointer";
    }

    int arg_num = atoi(builtin.ident.substr(4).c_str());
    Value *ctx = b_.CreatePointerCast(ctx_, b_.getInt64Ty()->getPointerTo());
    Value *sp = b_.CreateLoad(b_.getInt64Ty(),
                              b_.CreateGEP(ctx, b_.getInt64(sp_offset)),
                              "reg_sp");
    dyn_cast<LoadInst>(sp)->setVolatile(true);
    AllocaInst *dst = b_.CreateAllocaBPF(builtin.type, builtin.ident);
    Value *src = b_.CreateAdd(sp,
                              b_.getInt64((arg_num + arch::arg_stack_offset()) *
                                          sizeof(uintptr_t)));
    b_.CreateProbeRead(ctx_, dst, 8, src, builtin.type.GetAS(), builtin.loc);
    expr_ = b_.CreateLoad(dst);
    b_.CreateLifetimeEnd(dst);
  }
  else if (builtin.ident == "probe")
  {
    auto begin = bpftrace_.probe_ids_.begin();
    auto end = bpftrace_.probe_ids_.end();
    auto found = std::find(begin, end, probefull_);
    if (found == end) {
      bpftrace_.probe_ids_.push_back(probefull_);
      builtin.probe_id = bpftrace_.next_probe_id();
    } else {
      builtin.probe_id = std::distance(begin, found);
    }
    expr_ = b_.getInt64(builtin.probe_id);
  }
  else if (builtin.ident == "args" || builtin.ident == "ctx")
  {
    // ctx is undocumented builtin: for debugging
    // ctx_ is casted to int for arithmetic operation
    // it will be casted to a pointer when loading
    expr_ = b_.CreatePtrToInt(ctx_, b_.getInt64Ty());
  }
  else if (builtin.ident == "cpid")
  {
    pid_t cpid = bpftrace_.child_->pid();
    if (cpid < 1) {
      LOG(FATAL) << "BUG: Invalid cpid: " << cpid;
    }
    expr_ = b_.getInt64(cpid);
  }
  else
  {
    LOG(FATAL) << "unknown builtin \"" << builtin.ident << "\"";
  }
}

void CodegenLLVM::visit(Call &call)
{
  if (call.func == "count")
  {
    Map &map = *call.map;
    AllocaInst *key = getMapKey(map);
    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");
    b_.CreateStore(b_.CreateAdd(oldval, b_.getInt64(1)), newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);

    // oldval can only be an integer so won't be in memory and doesn't need lifetime end
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "sum")
  {
    Map &map = *call.map;
    AllocaInst *key = getMapKey(map);
    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");

    auto scoped_del = accept(call.vargs->front().get());
    // promote int to 64-bit
    expr_ = b_.CreateIntCast(expr_,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    b_.CreateStore(b_.CreateAdd(expr_, oldval), newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);

    // oldval can only be an integer so won't be in memory and doesn't need lifetime end
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "min")
  {
    Map &map = *call.map;
    AllocaInst *key = getMapKey(map);
    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");

    // Store the max of (0xffffffff - val), so that our SGE comparison with uninitialized
    // elements will always store on the first occurrence. Revent this later when printing.
    Function *parent = b_.GetInsertBlock()->getParent();
    auto scoped_del = accept(call.vargs->front().get());
    // promote int to 64-bit
    expr_ = b_.CreateIntCast(expr_,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    Value *inverted = b_.CreateSub(b_.getInt64(0xffffffff), expr_);
    BasicBlock *lt = BasicBlock::Create(module_->getContext(), "min.lt", parent);
    BasicBlock *ge = BasicBlock::Create(module_->getContext(), "min.ge", parent);
    b_.CreateCondBr(b_.CreateICmpSGE(inverted, oldval), ge, lt);

    b_.SetInsertPoint(ge);
    b_.CreateStore(inverted, newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);
    b_.CreateBr(lt);

    b_.SetInsertPoint(lt);
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "max")
  {
    Map &map = *call.map;
    AllocaInst *key = getMapKey(map);
    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");

    Function *parent = b_.GetInsertBlock()->getParent();
    auto scoped_del = accept(call.vargs->front().get());
    // promote int to 64-bit
    expr_ = b_.CreateIntCast(expr_,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    BasicBlock *lt = BasicBlock::Create(module_->getContext(), "min.lt", parent);
    BasicBlock *ge = BasicBlock::Create(module_->getContext(), "min.ge", parent);
    b_.CreateCondBr(b_.CreateICmpSGE(expr_, oldval), ge, lt);

    b_.SetInsertPoint(ge);
    b_.CreateStore(expr_, newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);
    b_.CreateBr(lt);

    b_.SetInsertPoint(lt);
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "avg" || call.func == "stats")
  {
    // avg stores the count and total in a hist map using indexes 0 and 1
    // respectively, and the calculation is made when printing.
    Map &map = *call.map;

    AllocaInst *count_key = getHistMapKey(map, b_.getInt64(0));
    Value *count_old = b_.CreateMapLookupElem(ctx_, map, count_key, call.loc);
    AllocaInst *count_new = b_.CreateAllocaBPF(map.type, map.ident + "_num");
    b_.CreateStore(b_.CreateAdd(count_old, b_.getInt64(1)), count_new);
    b_.CreateMapUpdateElem(ctx_, map, count_key, count_new, call.loc);
    b_.CreateLifetimeEnd(count_key);
    b_.CreateLifetimeEnd(count_new);

    AllocaInst *total_key = getHistMapKey(map, b_.getInt64(1));
    Value *total_old = b_.CreateMapLookupElem(ctx_, map, total_key, call.loc);
    AllocaInst *total_new = b_.CreateAllocaBPF(map.type, map.ident + "_val");
    auto scoped_del = accept(call.vargs->front().get());
    // promote int to 64-bit
    expr_ = b_.CreateIntCast(expr_,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    b_.CreateStore(b_.CreateAdd(expr_, total_old), total_new);
    b_.CreateMapUpdateElem(ctx_, map, total_key, total_new, call.loc);
    b_.CreateLifetimeEnd(total_key);
    b_.CreateLifetimeEnd(total_new);

    expr_ = nullptr;
  }
  else if (call.func == "hist")
  {
    if (!log2_func_)
      log2_func_ = createLog2Function();

    Map &map = *call.map;
    auto scoped_del = accept(call.vargs->front().get());
    // promote int to 64-bit
    expr_ = b_.CreateIntCast(expr_,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    Value *log2 = b_.CreateCall(log2_func_, expr_, "log2");
    AllocaInst *key = getHistMapKey(map, log2);

    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");
    b_.CreateStore(b_.CreateAdd(oldval, b_.getInt64(1)), newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);

    // oldval can only be an integer so won't be in memory and doesn't need lifetime end
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "lhist")
  {
    if (!linear_func_)
      linear_func_ = createLinearFunction();

    Map &map = *call.map;
    auto scoped_del = accept(call.vargs->front().get());

    // prepare arguments
    Integer *value_arg = static_cast<Integer *>(call.vargs->at(0).get());
    Integer *min_arg = static_cast<Integer *>(call.vargs->at(1).get());
    Integer *max_arg = static_cast<Integer *>(call.vargs->at(2).get());
    Integer *step_arg = static_cast<Integer *>(call.vargs->at(3).get());
    Value *value, *min, *max, *step;
    auto scoped_del_value_arg = accept(value_arg);
    value = expr_;
    auto scoped_del_min_arg = accept(min_arg);
    min = expr_;
    auto scoped_del_max_arg = accept(max_arg);
    max = expr_;
    auto scoped_del_step_arg = accept(step_arg);
    step = expr_;

    // promote int to 64-bit
    value = b_.CreateIntCast(value,
                             b_.getInt64Ty(),
                             call.vargs->front()->type.IsSigned());
    min = b_.CreateIntCast(min, b_.getInt64Ty(), false);
    max = b_.CreateIntCast(max, b_.getInt64Ty(), false);
    step = b_.CreateIntCast(step, b_.getInt64Ty(), false);

    Value *linear = b_.CreateCall(linear_func_,
                                  { value, min, max, step },
                                  "linear");

    AllocaInst *key = getHistMapKey(map, linear);

    Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, call.loc);
    AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_val");
    b_.CreateStore(b_.CreateAdd(oldval, b_.getInt64(1)), newval);
    b_.CreateMapUpdateElem(ctx_, map, key, newval, call.loc);

    // oldval can only be an integer so won't be in memory and doesn't need lifetime end
    b_.CreateLifetimeEnd(key);
    b_.CreateLifetimeEnd(newval);
    expr_ = nullptr;
  }
  else if (call.func == "delete")
  {
    auto &arg = *call.vargs->at(0);
    auto &map = static_cast<Map&>(arg);
    AllocaInst *key = getMapKey(map);
    b_.CreateMapDeleteElem(ctx_, map, key, call.loc);
    b_.CreateLifetimeEnd(key);
    expr_ = nullptr;
  }
  else if (call.func == "str")
  {
    AllocaInst *strlen = b_.CreateAllocaBPF(b_.getInt64Ty(), "strlen");
    b_.CREATE_MEMSET(strlen, b_.getInt8(0), sizeof(uint64_t), 1);
    if (call.vargs->size() > 1) {
      auto scoped_del = accept(call.vargs->at(1).get());
      Value *proposed_strlen = b_.CreateAdd(expr_, b_.getInt64(1)); // add 1 to accommodate probe_read_str's null byte

      // largest read we'll allow = our global string buffer size
      Value *max = b_.getInt64(bpftrace_.strlen_);
      // integer comparison: unsigned less-than-or-equal-to
      CmpInst::Predicate P = CmpInst::ICMP_ULE;
      // check whether proposed_strlen is less-than-or-equal-to maximum
      Value *Cmp = b_.CreateICmp(P, proposed_strlen, max, "str.min.cmp");
      // select proposed_strlen if it's sufficiently low, otherwise choose maximum
      Value *Select = b_.CreateSelect(Cmp, proposed_strlen, max, "str.min.select");
      b_.CreateStore(Select, strlen);
    } else {
      b_.CreateStore(b_.getInt64(bpftrace_.strlen_), strlen);
    }
    AllocaInst *buf = b_.CreateAllocaBPF(bpftrace_.strlen_, "str");
    b_.CREATE_MEMSET(buf, b_.getInt8(0), bpftrace_.strlen_, 1);
    auto &arg0 = call.vargs->front();
    auto scoped_del = accept(call.vargs->front().get());
    b_.CreateProbeReadStr(
        ctx_, buf, b_.CreateLoad(strlen), expr_, arg0->type.GetAS(), call.loc);
    b_.CreateLifetimeEnd(strlen);

    expr_ = buf;
    expr_deleter_ = [this,buf]() { b_.CreateLifetimeEnd(buf); };
  }
  else if (call.func == "buf")
  {
    Value *max_length = b_.getInt64(bpftrace_.strlen_);
    size_t fixed_buffer_length = bpftrace_.strlen_;
    Value *length;

    if (call.vargs->size() > 1)
    {
      auto &arg = *call.vargs->at(1);
      auto scoped_del = accept(&arg);

      Value *proposed_length = expr_;
      Value *cmp = b_.CreateICmp(
          CmpInst::ICMP_ULE, proposed_length, max_length, "length.cmp");
      length = b_.CreateSelect(
          cmp, proposed_length, max_length, "length.select");

      if (arg.is_literal)
        fixed_buffer_length = static_cast<Integer &>(arg).n;
    }
    else
    {
      auto &arg = *call.vargs->at(0);
      fixed_buffer_length = arg.type.GetNumElements() *
                            arg.type.GetElementTy()->size;
      length = b_.getInt8(fixed_buffer_length);
    }

    auto elements = AsyncEvent::Buf().asLLVMType(b_, fixed_buffer_length);
    char dynamic_sized_struct_name[30];
    sprintf(dynamic_sized_struct_name, "buffer_%ld_t", fixed_buffer_length);
    StructType *buf_struct = b_.GetStructType(dynamic_sized_struct_name,
                                              elements,
                                              false);
    AllocaInst *buf = b_.CreateAllocaBPF(buf_struct, "buffer");

    Value *buf_len_offset = b_.CreateGEP(buf,
                                         { b_.getInt32(0), b_.getInt32(0) });
    length = b_.CreateIntCast(length, buf_struct->getElementType(0), false);
    b_.CreateStore(length, buf_len_offset);

    Value *buf_data_offset = b_.CreateGEP(buf,
                                          { b_.getInt32(0), b_.getInt32(1) });
    b_.CREATE_MEMSET(buf_data_offset,
                     b_.GetIntSameSize(0, elements.at(0)),
                     fixed_buffer_length,
                     1);

    auto scoped_del = accept(call.vargs->front().get());
    auto &arg0 = call.vargs->front();
    b_.CreateProbeRead(ctx_,
                       static_cast<AllocaInst *>(buf_data_offset),
                       length,
                       expr_,
                       arg0->type.GetAS(),
                       call.loc);

    expr_ = buf;
    expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
  }
  else if (call.func == "kaddr")
  {
    uint64_t addr;
    auto &name = static_cast<String&>(*call.vargs->at(0)).str;
    addr = bpftrace_.resolve_kname(name);
    expr_ = b_.getInt64(addr);
  }
  else if (call.func == "uaddr")
  {
    auto &name = static_cast<String&>(*call.vargs->at(0)).str;
    struct symbol sym = {};
    int err =
        bpftrace_.resolve_uname(name, &sym, current_attach_point_->target);
    if (err < 0 || sym.address == 0)
      throw std::runtime_error("Could not resolve symbol: " +
                               current_attach_point_->target + ":" + name);
    expr_ = b_.getInt64(sym.address);
  }
  else if (call.func == "cgroupid")
  {
    uint64_t cgroupid;
    auto &path = static_cast<String&>(*call.vargs->at(0)).str;
    cgroupid = bpftrace_.resolve_cgroupid(path);
    expr_ = b_.getInt64(cgroupid);
  }
  else if (call.func == "join")
  {
    auto &arg0 = call.vargs->front();
    auto scoped_del = accept(arg0.get());
    auto addrspace = arg0->type.GetAS();
    AllocaInst *first = b_.CreateAllocaBPF(b_.getInt64Ty(),
                                           call.func + "_first");
    AllocaInst *second = b_.CreateAllocaBPF(b_.getInt64Ty(),
                                            call.func + "_second");
    Value *perfdata = b_.CreateGetJoinMap(ctx_, call.loc);
    Function *parent = b_.GetInsertBlock()->getParent();

    BasicBlock *zero = BasicBlock::Create(module_->getContext(),
                                          "joinzero",
                                          parent);
    BasicBlock *notzero = BasicBlock::Create(module_->getContext(),
                                             "joinnotzero",
                                             parent);

    b_.CreateCondBr(b_.CreateICmpNE(perfdata,
                                    ConstantExpr::getCast(Instruction::IntToPtr,
                                                          b_.getInt64(0),
                                                          b_.getInt8PtrTy()),
                                    "joinzerocond"),
                    notzero,
                    zero);

    // arg0
    b_.SetInsertPoint(notzero);
    b_.CreateStore(b_.getInt64(asyncactionint(AsyncAction::join)), perfdata);
    b_.CreateStore(b_.getInt64(join_id_),
                   b_.CreateGEP(perfdata, b_.getInt64(8)));
    join_id_++;
    AllocaInst *arr = b_.CreateAllocaBPF(b_.getInt64Ty(), call.func + "_r0");
    b_.CreateProbeRead(ctx_, arr, 8, expr_, addrspace, call.loc);
    b_.CreateProbeReadStr(ctx_,
                          b_.CreateAdd(perfdata, b_.getInt64(8 + 8)),
                          bpftrace_.join_argsize_,
                          b_.CreateLoad(arr),
                          addrspace,
                          call.loc);

    for (unsigned int i = 1; i < bpftrace_.join_argnum_; i++)
    {
      // argi
      b_.CreateStore(b_.CreateAdd(expr_, b_.getInt64(8 * i)), first);
      b_.CreateProbeRead(
          ctx_, second, 8, b_.CreateLoad(first), addrspace, call.loc);
      b_.CreateProbeReadStr(
          ctx_,
          b_.CreateAdd(perfdata,
                       b_.getInt64(8 + 8 + i * bpftrace_.join_argsize_)),
          bpftrace_.join_argsize_,
          b_.CreateLoad(second),
          addrspace,
          call.loc);
    }

    // emit
    b_.CreatePerfEventOutput(
        ctx_,
        perfdata,
        8 + 8 + bpftrace_.join_argnum_ * bpftrace_.join_argsize_);

    b_.CreateBr(zero);

    // done
    b_.SetInsertPoint(zero);
    expr_ = nullptr;
  }
  else if (call.func == "ksym")
  {
    // We want expr_ to just pass through from the child node - don't set it here
    auto scoped_del = accept(call.vargs->front().get());
  }
  else if (call.func == "usym")
  {
    auto scoped_del = accept(call.vargs->front().get());
    expr_ = b_.CreateUSym(expr_);
  }
  else if (call.func == "ntop")
  {
    // struct {
    //   int af_type;
    //   union {
    //     char[4] inet4;
    //     char[16] inet6;
    //   }
    // }
    //}
    std::vector<llvm::Type *> elements = { b_.getInt64Ty(),
                                           ArrayType::get(b_.getInt8Ty(), 16) };
    StructType *inet_struct = b_.GetStructType("inet_t", elements, false);

    AllocaInst *buf = b_.CreateAllocaBPF(inet_struct, "inet");

    Value *af_offset = b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) });
    Value *af_type;

    auto inet = call.vargs->at(0).get();
    if (call.vargs->size() == 1)
    {
      if (inet->type.IsIntegerTy() || inet->type.size == 4)
      {
        af_type = b_.getInt64(AF_INET);
      }
      else
      {
        af_type = b_.getInt64(AF_INET6);
      }
    }
    else
    {
      inet = call.vargs->at(1).get();
      auto scoped_del = accept(call.vargs->at(0).get());
      af_type = b_.CreateIntCast(expr_, b_.getInt64Ty(), true);
    }
    b_.CreateStore(af_type, af_offset);

    Value *inet_offset = b_.CreateGEP(buf, {b_.getInt32(0), b_.getInt32(1)});
    b_.CREATE_MEMSET(inet_offset, b_.getInt8(0), 16, 1);

    auto scoped_del = accept(inet);
    if (inet->type.IsArray())
    {
      b_.CreateProbeRead(ctx_,
                         static_cast<AllocaInst *>(inet_offset),
                         inet->type.size,
                         expr_,
                         inet->type.GetAS(),
                         call.loc);
    }
    else
    {
      b_.CreateStore(b_.CreateIntCast(expr_, b_.getInt32Ty(), false),
                     b_.CreatePointerCast(inet_offset,
                                          b_.getInt32Ty()->getPointerTo()));
    }

    expr_ = buf;
    expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
  }
  else if (call.func == "reg")
  {
    auto &reg_name = static_cast<String&>(*call.vargs->at(0)).str;
    int offset = arch::offset(reg_name);
    if (offset == -1)
    {
      LOG(FATAL) << "negative offset on reg() call";
    }

    Value *ctx = b_.CreatePointerCast(ctx_, b_.getInt64Ty()->getPointerTo());
    expr_ = b_.CreateLoad(b_.getInt64Ty(),
                          b_.CreateGEP(ctx, b_.getInt64(offset)),
                          call.func + "_" + reg_name);
    dyn_cast<LoadInst>(expr_)->setVolatile(true);
  }
  else if (call.func == "printf")
  {
    createFormatStringCall(call,
                           printf_id_,
                           bpftrace_.printf_args_,
                           "printf",
                           AsyncAction::printf);
  }
  else if (call.func == "system")
  {
    createFormatStringCall(call,
                           system_id_,
                           bpftrace_.system_args_,
                           "system",
                           AsyncAction::syscall);
  }
  else if (call.func == "cat")
  {
    createFormatStringCall(
        call, cat_id_, bpftrace_.cat_args_, "cat", AsyncAction::cat);
  }
  else if (call.func == "exit")
  {
    /*
     * perf event output has: uint64_t asyncaction_id
     * The asyncaction_id informs user-space that this is not a printf(), but is a
     * special asynchronous action. The ID maps to exit().
     */
    AllocaInst *perfdata = b_.CreateAllocaBPF(b_.getInt64Ty(), "perfdata");
    b_.CreateStore(b_.getInt64(asyncactionint(AsyncAction::exit)), perfdata);
    b_.CreatePerfEventOutput(ctx_, perfdata, sizeof(uint64_t));
    b_.CreateLifetimeEnd(perfdata);
    expr_ = nullptr;
    b_.CreateRet(ConstantInt::get(module_->getContext(), APInt(64, 0)));

    // create an unreachable basic block for all the "dead instructions" that
    // may come after exit(). If we don't, LLVM will emit the instructions
    // leading to a `unreachable insn` warning from the verifier
    BasicBlock *deadcode = BasicBlock::Create(module_->getContext(),
                                              "deadcode",
                                              b_.GetInsertBlock()->getParent());
    b_.SetInsertPoint(deadcode);
  }
  else if (call.func == "print")
  {
    if (call.vargs->at(0)->is_map)
      createPrintMapCall(call);
    else
      createPrintNonMapCall(call, non_map_print_id_);
  }
  else if (call.func == "clear" || call.func == "zero")
  {
    auto elements = AsyncEvent::MapEvent().asLLVMType(b_);
    StructType *event_struct = b_.GetStructType(call.func + "_t",
                                                elements,
                                                true);

    auto &arg = *call.vargs->at(0);
    auto &map = static_cast<Map&>(arg);

    AllocaInst *buf = b_.CreateAllocaBPF(event_struct,
                                         call.func + "_" + map.ident);

    auto aa_ptr = b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) });
    if (call.func == "clear")
      b_.CreateStore(b_.GetIntSameSize(asyncactionint(AsyncAction::clear),
                                       elements.at(0)),
                     aa_ptr);
    else
      b_.CreateStore(b_.GetIntSameSize(asyncactionint(AsyncAction::zero),
                                       elements.at(0)),
                     aa_ptr);

    auto id = bpftrace_.maps[map.ident].value()->id;
    auto *ident_ptr = b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(1) });
    b_.CreateStore(b_.GetIntSameSize(id, elements.at(1)), ident_ptr);

    b_.CreatePerfEventOutput(ctx_, buf, getStructSize(event_struct));
    b_.CreateLifetimeEnd(buf);
    expr_ = nullptr;
  }
  else if (call.func == "time")
  {
    auto elements = AsyncEvent::Time().asLLVMType(b_);
    StructType *time_struct = b_.GetStructType(call.func + "_t",
                                               elements,
                                               true);

    AllocaInst *buf = b_.CreateAllocaBPF(time_struct, call.func + "_t");

    b_.CreateStore(b_.GetIntSameSize(asyncactionint(AsyncAction::time),
                                     elements.at(0)),
                   b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) }));

    b_.CreateStore(b_.GetIntSameSize(time_id_, elements.at(1)),
                   b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(1) }));

    time_id_++;
    b_.CreatePerfEventOutput(ctx_, buf, getStructSize(time_struct));
    b_.CreateLifetimeEnd(buf);
    expr_ = nullptr;
  }
  else if (call.func == "strftime")
  {
    auto elements = AsyncEvent::Strftime().asLLVMType(b_);
    StructType *strftime_struct = b_.GetStructType(call.func + "_t",
                                                   elements,
                                                   true);

    AllocaInst *buf = b_.CreateAllocaBPF(strftime_struct, call.func + "_args");
    b_.CreateStore(b_.GetIntSameSize(strftime_id_, elements.at(0)),
                   b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) }));
    strftime_id_++;
    Expression *arg = call.vargs->at(1).get();
    auto scoped_del = accept(arg);
    b_.CreateStore(expr_,
                   b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(1) }));
    expr_ = buf;
  }
  else if (call.func == "kstack" || call.func == "ustack")
  {
    Value *stackid = b_.CreateGetStackId(
        ctx_, call.func == "ustack", call.type.stack_type, call.loc);
    // Kernel stacks should not be differentiated by tid, since the kernel
    // address space is the same between pids (and when aggregating you *want*
    // to be able to correlate between pids in most cases). User-space stacks
    // are special because of ASLR and so we do usym()-style packing.
    if (call.func == "ustack")
    {
      // pack uint64_t with: (uint32_t)stack_id, (uint32_t)pid
      Value *pidhigh = b_.CreateShl(b_.CreateGetPidTgid(), 32);
      stackid = b_.CreateOr(stackid, pidhigh);
    }
    expr_ = stackid;
  }
  else if (call.func == "signal") {
    // int bpf_send_signal(u32 sig)
    auto &arg = *call.vargs->at(0);
    if (arg.type.IsStringTy())
    {
      auto signame = static_cast<String&>(arg).str;
      int sigid = signal_name_to_num(signame);
      // Should be caught in semantic analyser
      if (sigid < 1) {
        LOG(FATAL) << "BUG: Invalid signal ID for \"" << signame << "\"";
      }
      b_.CreateSignal(ctx_, b_.getInt32(sigid), call.loc);
      return;
    }
    auto scoped_del = accept(&arg);
    expr_ = b_.CreateIntCast(expr_, b_.getInt32Ty(), arg.type.IsSigned());
    b_.CreateSignal(ctx_, expr_, call.loc);
  }
  else if (call.func == "sizeof")
  {
    expr_ = b_.getInt64(call.vargs->at(0)->type.size);
  }
  else if (call.func == "strncmp") {
    uint64_t size = static_cast<Integer *>(call.vargs->at(2).get())->n;
    const auto& left_arg = call.vargs->at(0);
    const auto& right_arg = call.vargs->at(1);
    auto left_as = left_arg->type.GetAS();
    auto right_as = right_arg->type.GetAS();

    // If one of the strings is fixed, we can avoid storing the
    // literal in memory by calling a different function.
    if (right_arg->is_literal)
    {
      auto scoped_del = accept(left_arg.get());
      Value *left_string = expr_;
      const auto &string_literal = static_cast<String *>(right_arg.get())->str;
      expr_ = b_.CreateStrncmp(
          ctx_, left_string, left_as, string_literal, size, call.loc, false);
    }
    else if (left_arg->is_literal)
    {
      auto scoped_del = accept(right_arg.get());
      Value *right_string = expr_;
      const auto &string_literal = static_cast<String *>(left_arg.get())->str;
      expr_ = b_.CreateStrncmp(
          ctx_, right_string, right_as, string_literal, size, call.loc, false);
    }
    else
    {
      auto scoped_del_right = accept(right_arg.get());
      Value *right_string = expr_;
      auto scoped_del_left = accept(left_arg.get());
      Value *left_string = expr_;
      expr_ = b_.CreateStrncmp(ctx_,
                               left_string,
                               left_as,
                               right_string,
                               right_as,
                               size,
                               call.loc,
                               false);
    }
  }
  else if (call.func == "override")
  {
    // int bpf_override(struct pt_regs *regs, u64 rc)
    // returns: 0
    auto &arg = *call.vargs->at(0);
    auto scoped_del = accept(&arg);
    expr_ = b_.CreateIntCast(expr_, b_.getInt64Ty(), arg.type.IsSigned());
    b_.CreateOverrideReturn(ctx_, expr_);
  }
  else if (call.func == "kptr" || call.func == "uptr")
  {
    auto &arg = call.vargs->at(0);
    auto scoped_del = accept(arg.get());
  }
  else
  {
    LOG(FATAL) << "missing codegen for function \"" << call.func << "\"";
  }
}

void CodegenLLVM::visit(Map &map)
{
  AllocaInst *key = getMapKey(map);
  Value *value = b_.CreateMapLookupElem(ctx_, map, key, map.loc);
  expr_ = value;

  if (dyn_cast<AllocaInst>(value))
    expr_deleter_ = [this, value]() { b_.CreateLifetimeEnd(value); };
  b_.CreateLifetimeEnd(key);
}

void CodegenLLVM::visit(Variable &var)
{
  if (needMemcpy(var.type))
  {
    expr_ = variables_[var.ident];
  }
  else
  {
    expr_ = b_.CreateLoad(variables_[var.ident]);
  }
}

void CodegenLLVM::visit(Binop &binop)
{
  // Handle && and || separately so short circuiting works
  if (binop.op == bpftrace::Parser::token::LAND)
  {
    expr_ = createLogicalAnd(binop);
    return;
  }
  else if (binop.op == bpftrace::Parser::token::LOR)
  {
    expr_ = createLogicalOr(binop);
    return;
  }

  SizedType &type = binop.left->type;
  if (type.IsStringTy())
  {

    if (binop.op != bpftrace::Parser::token::EQ && binop.op != bpftrace::Parser::token::NE) {
      LOG(FATAL) << "missing codegen to string operator \"" << opstr(binop)
                 << "\"";
    }

    std::string string_literal;

    // strcmp returns 0 when strings are equal
    bool inverse = binop.op == bpftrace::Parser::token::EQ;

    auto left_as = binop.left->type.GetAS();
    auto right_as = binop.right->type.GetAS();

    // If one of the strings is fixed, we can avoid storing the
    // literal in memory by calling a different function.
    if (binop.right->is_literal)
    {
      auto scoped_del = accept(binop.left.get());
      string_literal = static_cast<String *>(binop.right.get())->str;
      expr_ = b_.CreateStrcmp(
          ctx_, expr_, left_as, string_literal, binop.loc, inverse);
    }
    else if (binop.left->is_literal)
    {
      auto scoped_del = accept(binop.right.get());
      string_literal = static_cast<String *>(binop.left.get())->str;
      expr_ = b_.CreateStrcmp(
          ctx_, expr_, right_as, string_literal, binop.loc, inverse);
    }
    else
    {
      auto scoped_del_right = accept(binop.right.get());
      Value * right_string = expr_;

      auto scoped_del_left = accept(binop.left.get());
      Value * left_string = expr_;

      size_t len = std::min(binop.left->type.size, binop.right->type.size);
      expr_ = b_.CreateStrncmp(ctx_,
                               left_string,
                               left_as,
                               right_string,
                               right_as,
                               len + 1,
                               binop.loc,
                               inverse);
    }
  }
  else if (type.IsBufferTy())
  {
    if (binop.op != bpftrace::Parser::token::EQ &&
        binop.op != bpftrace::Parser::token::NE)
    {
      LOG(FATAL) << "missing codegen to buffer operator \"" << opstr(binop)
                 << "\"";
    }

    std::string string_literal("");

    // strcmp returns 0 when strings are equal
    bool inverse = binop.op == bpftrace::Parser::token::EQ;

    auto scoped_del_right = accept(binop.right.get());
    Value *right_string = expr_;
    auto right_as = binop.right->type.GetAS();

    auto scoped_del_left = accept(binop.left.get());
    Value *left_string = expr_;
    auto left_as = binop.left->type.GetAS();

    size_t len = std::min(binop.left->type.size, binop.right->type.size);
    expr_ = b_.CreateStrncmp(ctx_,
                             left_string,
                             left_as,
                             right_string,
                             right_as,
                             len,
                             binop.loc,
                             inverse);
  }
  else
  {
    Value *lhs, *rhs;
    auto scoped_del_left = accept(binop.left.get());
    lhs = expr_;
    auto scoped_del_right = accept(binop.right.get());
    rhs = expr_;

    bool lsign = binop.left->type.IsSigned();
    bool rsign = binop.right->type.IsSigned();
    bool do_signed = lsign && rsign;
    // promote int to 64-bit
    lhs = b_.CreateIntCast(lhs, b_.getInt64Ty(), lsign);
    rhs = b_.CreateIntCast(rhs, b_.getInt64Ty(), rsign);

    switch (binop.op) {
      case bpftrace::Parser::token::EQ:    expr_ = b_.CreateICmpEQ (lhs, rhs); break;
      case bpftrace::Parser::token::NE:    expr_ = b_.CreateICmpNE (lhs, rhs); break;
      case bpftrace::Parser::token::LE: {
        expr_ = do_signed ? b_.CreateICmpSLE(lhs, rhs) : b_.CreateICmpULE(lhs, rhs);
        break;
      }
      case bpftrace::Parser::token::GE: {
        expr_ = do_signed ? b_.CreateICmpSGE(lhs, rhs) : b_.CreateICmpUGE(lhs, rhs);
        break;
      }
      case bpftrace::Parser::token::LT: {
        expr_ = do_signed ? b_.CreateICmpSLT(lhs, rhs) : b_.CreateICmpULT(lhs, rhs);
        break;
      }
      case bpftrace::Parser::token::GT: {
        expr_ = do_signed ? b_.CreateICmpSGT(lhs, rhs) : b_.CreateICmpUGT(lhs, rhs);
        break;
      }
      case bpftrace::Parser::token::LEFT:  expr_ = b_.CreateShl    (lhs, rhs); break;
      case bpftrace::Parser::token::RIGHT: expr_ = b_.CreateLShr   (lhs, rhs); break;
      case bpftrace::Parser::token::PLUS:  expr_ = b_.CreateAdd    (lhs, rhs); break;
      case bpftrace::Parser::token::MINUS: expr_ = b_.CreateSub    (lhs, rhs); break;
      case bpftrace::Parser::token::MUL:   expr_ = b_.CreateMul    (lhs, rhs); break;
      case bpftrace::Parser::token::DIV:   expr_ = b_.CreateUDiv   (lhs, rhs); break;
      case bpftrace::Parser::token::MOD: {
        // Always do an unsigned modulo operation here even if `do_signed`
        // is true. bpf instruction set does not support signed division.
        // We already warn in the semantic analyser that signed modulo can
        // lead to undefined behavior (because we will treat it as unsigned).
        expr_ = b_.CreateURem(lhs, rhs);
        break;
      }
      case bpftrace::Parser::token::BAND:  expr_ = b_.CreateAnd    (lhs, rhs); break;
      case bpftrace::Parser::token::BOR:   expr_ = b_.CreateOr     (lhs, rhs); break;
      case bpftrace::Parser::token::BXOR:  expr_ = b_.CreateXor    (lhs, rhs); break;
      case bpftrace::Parser::token::LAND:
      case bpftrace::Parser::token::LOR:
        LOG(FATAL) << "\"" << opstr(binop) << "\" was handled earlier";
    }
  }
  // Using signed extension will result in -1 which will likely confuse users
  expr_ = b_.CreateIntCast(expr_, b_.getInt64Ty(), false);
}

static bool unop_skip_accept(Unop &unop)
{
  if (unop.expr->type.IsIntTy())
  {
    if (unop.op == bpftrace::Parser::token::INCREMENT ||
        unop.op == bpftrace::Parser::token::DECREMENT)
      return unop.expr->is_map || unop.expr->is_variable;
  }

  return false;
}

void CodegenLLVM::visit(Unop &unop)
{
  auto scoped_del = ScopedExprDeleter(nullptr);
  if (!unop_skip_accept(unop))
    scoped_del = accept(unop.expr.get());

  SizedType &type = unop.expr->type;
  if (type.IsIntegerTy())
  {
    switch (unop.op)
    {
      case bpftrace::Parser::token::LNOT:
      {
        auto ty = expr_->getType();
        Value *zero_value = Constant::getNullValue(ty);
        expr_ = b_.CreateICmpEQ(expr_, zero_value);
        // CreateICmpEQ() returns 1-bit integer
        // Cast it to the same type of the operand
        // Use unsigned extention, otherwise !0 becomes -1
        expr_ = b_.CreateIntCast(expr_, ty, false);
        break;
      }
      case bpftrace::Parser::token::BNOT: expr_ = b_.CreateNot(expr_); break;
      case bpftrace::Parser::token::MINUS: expr_ = b_.CreateNeg(expr_); break;
      case bpftrace::Parser::token::INCREMENT:
      case bpftrace::Parser::token::DECREMENT:
      {
        bool is_increment = unop.op == bpftrace::Parser::token::INCREMENT;

        if (unop.expr->is_map)
        {
          Map &map = static_cast<Map&>(*unop.expr);
          AllocaInst *key = getMapKey(map);
          Value *oldval = b_.CreateMapLookupElem(ctx_, map, key, unop.loc);
          AllocaInst *newval = b_.CreateAllocaBPF(map.type, map.ident + "_newval");
          if (is_increment)
            b_.CreateStore(b_.CreateAdd(oldval, b_.getInt64(1)), newval);
          else
            b_.CreateStore(b_.CreateSub(oldval, b_.getInt64(1)), newval);
          b_.CreateMapUpdateElem(ctx_, map, key, newval, unop.loc);
          b_.CreateLifetimeEnd(key);

          if (unop.is_post_op)
            expr_ = oldval;
          else
            expr_ = b_.CreateLoad(newval);
          b_.CreateLifetimeEnd(newval);
        }
        else if (unop.expr->is_variable)
        {
          Variable &var = static_cast<Variable&>(*unop.expr);
          Value *oldval = b_.CreateLoad(variables_[var.ident]);
          Value *newval;
          if (is_increment)
            newval = b_.CreateAdd(oldval, b_.getInt64(1));
          else
            newval = b_.CreateSub(oldval, b_.getInt64(1));
          b_.CreateStore(newval, variables_[var.ident]);

          if (unop.is_post_op)
            expr_ = oldval;
          else
            expr_ = newval;
        }
        else
        {
          LOG(FATAL) << "invalid expression passed to " << opstr(unop);
        }
        break;
      }
      case bpftrace::Parser::token::MUL:
      {
        int size = type.size;
        if (type.IsPtrTy())
        {
          // When dereferencing a 32-bit integer, only read in 32-bits, etc.
          size = type.GetPointeeTy()->size;
        }
        AllocaInst *dst = b_.CreateAllocaBPF(SizedType(type.type, size), "deref");
        b_.CreateProbeRead(ctx_, dst, size, expr_, type.GetAS(), unop.loc);
        expr_ = b_.CreateIntCast(b_.CreateLoad(dst),
                                 b_.getInt64Ty(),
                                 type.IsSigned());
        b_.CreateLifetimeEnd(dst);
        break;
      }
    }
  }
  else if (type.IsPtrTy())
  {
    switch (unop.op)
    {
      case bpftrace::Parser::token::MUL:
      {
        if (unop.type.IsIntegerTy())
        {
          auto *et = type.GetPointeeTy();
          int size = et->GetIntBitWidth() / 8;
          AllocaInst *dst = b_.CreateAllocaBPF(*et, "deref");
          b_.CreateProbeRead(ctx_, dst, size, expr_, type.GetAS(), unop.loc);
          expr_ = b_.CreateIntCast(b_.CreateLoad(dst),
                                   b_.getInt64Ty(),
                                   unop.type.IsSigned());
          b_.CreateLifetimeEnd(dst);
        }
        break;
      }
      default:; // Do nothing
    }
  }
  else
  {
    LOG(FATAL) << "invalid type (" << type << ") passed to unary operator \""
               << opstr(unop) << "\"";
  }
}

void CodegenLLVM::visit(Ternary &ternary)
{
  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *left_block = BasicBlock::Create(module_->getContext(), "left", parent);
  BasicBlock *right_block = BasicBlock::Create(module_->getContext(), "right", parent);
  BasicBlock *done = BasicBlock::Create(module_->getContext(), "done", parent);
  // ordering of all the following statements is important
  Value *result = ternary.type.IsNoneTy()
                      ? nullptr
                      : b_.CreateAllocaBPF(ternary.type, "result");
  AllocaInst *buf = ternary.type.IsNoneTy()
                        ? nullptr
                        : b_.CreateAllocaBPF(ternary.type, "buf");
  Value *cond;
  auto scoped_del = accept(ternary.cond.get());
  cond = expr_;
  Value *zero_value = Constant::getNullValue(cond->getType());
  b_.CreateCondBr(b_.CreateICmpNE(cond, zero_value, "true_cond"),
                  left_block,
                  right_block);

  if (ternary.type.IsIntTy())
  {
    // fetch selected integer via CreateStore
    b_.SetInsertPoint(left_block);
    auto scoped_del_left = accept(ternary.left.get());
    expr_ = b_.CreateIntCast(expr_,
                             b_.GetType(ternary.type),
                             ternary.type.IsSigned());
    b_.CreateStore(expr_, result);
    b_.CreateBr(done);

    b_.SetInsertPoint(right_block);
    auto scoped_del_right = accept(ternary.right.get());
    expr_ = b_.CreateIntCast(expr_,
                             b_.GetType(ternary.type),
                             ternary.type.IsSigned());
    b_.CreateStore(expr_, result);
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    expr_ = b_.CreateLoad(result);
  }
  else if (ternary.type.IsStringTy())
  {
    // copy selected string via CreateMemCpy
    b_.SetInsertPoint(left_block);
    auto scoped_del_left = accept(ternary.left.get());
    b_.CREATE_MEMCPY(buf, expr_, ternary.type.size, 1);
    b_.CreateBr(done);

    b_.SetInsertPoint(right_block);
    auto scoped_del_right = accept(ternary.right.get());
    b_.CREATE_MEMCPY(buf, expr_, ternary.type.size, 1);
    b_.CreateBr(done);

    b_.SetInsertPoint(done);
    expr_ = buf;
    expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
  }
  else
  {
    // Type::none
    b_.SetInsertPoint(left_block);
    {
      auto scoped_del = accept(ternary.left.get());
    }
    b_.CreateBr(done);
    b_.SetInsertPoint(right_block);
    {
      auto scoped_del = accept(ternary.right.get());
    }
    b_.CreateBr(done);
    b_.SetInsertPoint(done);
    expr_ = nullptr;
  }
}

void CodegenLLVM::visit(FieldAccess &acc)
{
  SizedType &type = acc.expr->type;
  assert(type.IsRecordTy() || type.IsTupleTy());
  auto scoped_del = accept(acc.expr.get());

  bool is_ctx = type.IsCtxAccess();
  bool is_tparg = type.is_tparg;
  bool is_internal = type.is_internal;
  bool is_kfarg = type.is_kfarg;
  assert(type.IsRecordTy() || type.IsTupleTy());

  if (type.is_kfarg)
  {
    expr_ = b_.CreatKFuncArg(ctx_, acc.type, acc.field);
    return;
  }
  else if (type.IsTupleTy())
  {
    Value *src = b_.CreateGEP(expr_,
                              { b_.getInt32(0), b_.getInt32(acc.index) });
    SizedType &elem_type = type.tuple_elems[acc.index];

    if (shouldBeOnStackAlready(elem_type))
    {
      expr_ = src;
      // Extend lifetime of source buffer
      expr_deleter_ = scoped_del.disarm();
    }
    else
      expr_ = b_.CreateLoad(b_.GetType(elem_type), src);

    return;
  }

  std::string cast_type = is_tparg ? tracepoint_struct_ : type.GetName();
  Struct &cstruct = bpftrace_.structs_[cast_type];

  // This overwrites the stored type!
  type = CreateRecord(cstruct.size, cast_type);
  if (is_ctx)
    type.MarkCtxAccess();
  type.is_tparg = is_tparg;
  type.is_internal = is_internal;
  type.is_kfarg = is_kfarg;

  auto &field = cstruct.fields[acc.field];

  if (is_internal)
  {
    // The struct we are reading from has already been pulled into
    // BPF-memory, e.g. by being stored in a map.
    // Just read from the correct offset of expr_
    Value *src = b_.CreateGEP(expr_, {b_.getInt64(0), b_.getInt64(field.offset)});

    if (field.type.IsRecordTy())
    {
      // TODO This should be do-able without allocating more memory here
      AllocaInst *dst = b_.CreateAllocaBPF(field.type,
                                           "internal_" + type.GetName() + "." +
                                               acc.field);
      b_.CREATE_MEMCPY(dst, src, field.type.size, 1);
      expr_ = dst;
      expr_deleter_ = [this, dst]() { b_.CreateLifetimeEnd(dst); };
    }
    else if (field.type.IsStringTy() || field.type.IsBufferTy())
    {
      expr_ = src;
      // Extend lifetime of source buffer
      expr_deleter_ = scoped_del.disarm();
    }
    else
    {
      expr_ = b_.CreateLoad(b_.GetType(field.type), src);
    }
  }
  else
  {
    // The struct we are reading from has not been pulled into BPF-memory,
    // so expr_ will contain an external pointer to the start of the struct

    Value *src = b_.CreateAdd(expr_, b_.getInt64(field.offset));

    if (field.type.IsRecordTy())
    {
      // struct X
      // {
      //   struct Y y;
      // };
      //
      // We are trying to access an embedded struct, e.g. "x.y"
      //
      // Instead of copying the entire struct Y in, we'll just store it as a
      // pointer internally and dereference later when necessary.
      expr_ = src;
      // Extend lifetime of source buffer
      expr_deleter_ = scoped_del.disarm();
      return;
    }

    llvm::Type *field_ty = b_.GetType(field.type);
    if (field.type.IsArrayTy())
    {
      // For array types, we want to just pass pointer along,
      // since the offset of the field should be the start of the array.
      // The pointer will be dereferenced when the array is accessed by a []
      // operation
      expr_ = src;
      // Extend lifetime of source buffer
      expr_deleter_ = scoped_del.disarm();
    }
    else if (field.type.IsStringTy() || field.type.IsBufferTy())
    {
      AllocaInst *dst = b_.CreateAllocaBPF(field.type,
                                           type.GetName() + "." + acc.field);
      if (type.IsCtxAccess())
      {
        // Map functions only accept a pointer to a element in the stack
        // Copy data to avoid the above issue
        b_.CREATE_MEMCPY_VOLATILE(dst,
                                  b_.CreateIntToPtr(src,
                                                    field_ty->getPointerTo()),
                                  field.type.size,
                                  1);
      }
      else
      {
        b_.CreateProbeRead(
            ctx_, dst, field.type.size, src, type.GetAS(), acc.loc);
      }
      expr_ = dst;
      expr_deleter_ = [this, dst]() { b_.CreateLifetimeEnd(dst); };
    }
    else if (field.type.IsIntTy() && field.is_bitfield)
    {
      Value *raw;
      if (type.IsCtxAccess())
        raw = b_.CreateLoad(b_.CreateIntToPtr(src, field_ty->getPointerTo()),
                            true);
      else
      {
        AllocaInst *dst = b_.CreateAllocaBPF(field.type,
                                             type.GetName() + "." + acc.field);
        // memset so verifier doesn't complain about reading uninitialized stack
        b_.CREATE_MEMSET(dst, b_.getInt8(0), field.type.size, 1);
        b_.CreateProbeRead(
            ctx_, dst, field.bitfield.read_bytes, src, type.GetAS(), acc.loc);
        raw = b_.CreateLoad(dst);
        b_.CreateLifetimeEnd(dst);
      }
      Value *shifted = b_.CreateLShr(raw, field.bitfield.access_rshift);
      Value *masked = b_.CreateAnd(shifted, field.bitfield.mask);
      expr_ = masked;
    }
    else if ((field.type.IsIntTy() || field.type.IsPtrTy()) &&
             type.IsCtxAccess())
    {
      expr_ = b_.CreateLoad(b_.CreateIntToPtr(src, field_ty->getPointerTo()),
                            true);
      expr_ = b_.CreateIntCast(expr_, b_.getInt64Ty(), field.type.IsSigned());
    }
    else
    {
      AllocaInst *dst = b_.CreateAllocaBPF(field.type,
                                           type.GetName() + "." + acc.field);
      b_.CreateProbeRead(
          ctx_, dst, field.type.size, src, type.GetAS(), acc.loc);
      expr_ = b_.CreateIntCast(b_.CreateLoad(dst),
                               b_.getInt64Ty(),
                               field.type.IsSigned());
      b_.CreateLifetimeEnd(dst);
    }
  }
}

void CodegenLLVM::visit(ArrayAccess &arr)
{
  Value *array, *index, *offset;
  SizedType &type = arr.expr->type;
  size_t element_size = type.GetElementTy()->size;

  auto scoped_del_expr = accept(arr.expr.get());
  array = expr_;

  auto scoped_del_index = accept(arr.indexpr.get());

  index = b_.CreateIntCast(expr_, b_.getInt64Ty(), arr.expr->type.IsSigned());
  offset = b_.CreateMul(index, b_.getInt64(element_size));

  Value *src = b_.CreateAdd(array, offset);

  auto stype = *type.GetElementTy();

  if (stype.IsIntegerTy() || stype.IsPtrTy())
  {
    if (arr.expr->type.IsCtxAccess())
    {
      auto ty = b_.GetType(stype);
      expr_ = b_.CreateLoad(b_.CreateIntToPtr(src, ty->getPointerTo()), true);
    }
    else
    {
      AllocaInst *dst = b_.CreateAllocaBPF(stype, "array_access");
      b_.CreateProbeRead(ctx_, dst, element_size, src, type.GetAS(), arr.loc);
      expr_ = b_.CreateIntCast(b_.CreateLoad(dst),
                               b_.getInt64Ty(),
                               arr.expr->type.IsSigned());
      b_.CreateLifetimeEnd(dst);
    }
  }
  else
  {
    AllocaInst *dst = b_.CreateAllocaBPF(stype, "array_access");
    b_.CreateProbeRead(ctx_, dst, element_size, src, type.GetAS(), arr.loc);
    expr_ = dst;
    expr_deleter_ = [this, dst]() { b_.CreateLifetimeEnd(dst); };
  }
}

void CodegenLLVM::visit(Cast &cast)
{
  auto scoped_del = accept(cast.expr.get());
  if (cast.type.IsIntTy())
  {
    expr_ = b_.CreateIntCast(
        expr_, b_.getIntNTy(8 * cast.type.size), cast.type.IsSigned(), "cast");
  }
}

void CodegenLLVM::visit(Tuple &tuple)
{
  // Store elements on stack
  llvm::Type *tuple_ty = b_.GetType(tuple.type);
  AllocaInst *buf = b_.CreateAllocaBPF(tuple_ty, "tuple");
  for (size_t i = 0; i < tuple.elems->size(); ++i)
  {
    auto &elem = tuple.elems->at(i);
    auto scoped_del = accept(elem.get());

    Value *dst = b_.CreateGEP(buf, { b_.getInt32(0), b_.getInt32(i) });

    if (shouldBeOnStackAlready(elem->type))
      b_.CREATE_MEMCPY(dst, expr_, elem->type.size, 1);
    else
      b_.CreateStore(expr_, dst);
  }

  expr_ = buf;
  expr_deleter_ = [this, buf]() { b_.CreateLifetimeEnd(buf); };
}

void CodegenLLVM::visit(ExprStatement &expr)
{
  auto scoped_del = accept(expr.expr.get());
}

void CodegenLLVM::visit(AssignMapStatement &assignment)
{
  Map &map = *assignment.map;
  auto scoped_del = accept(assignment.expr.get());
  bool self_alloca = false;

  if (!expr_) // Some functions do the assignments themselves
    return;

  Value *val, *expr;
  expr = expr_;
  AllocaInst *key = getMapKey(map);
  if (shouldBeOnStackAlready(assignment.expr->type))
  {
    val = expr;
  }
  else if (map.type.IsRecordTy())
  {
    if (assignment.expr->type.is_internal)
    {
      val = expr;
    }
    else
    {
      // expr currently contains a pointer to the struct
      // We now want to read the entire struct in so we can save it
      AllocaInst *dst = b_.CreateAllocaBPF(map.type, map.ident + "_val");
      b_.CreateProbeRead(ctx_,
                         dst,
                         map.type.size,
                         expr,
                         assignment.expr->type.GetAS(),
                         assignment.loc);
      val = dst;
      self_alloca = true;
    }
  }
  else if (map.type.IsPtrTy())
  {
    // expr currently contains a pointer to the struct
    // and that's what we are saving
    AllocaInst *dst = b_.CreateAllocaBPF(map.type, map.ident + "_ptr");
    b_.CreateStore(expr, dst);
    val = dst;
    self_alloca = true;
  }
  else
  {
    if (map.type.IsIntTy())
    {
      // Integers are always stored as 64-bit in map values
      expr = b_.CreateIntCast(expr, b_.getInt64Ty(), map.type.IsSigned());
    }
    val = b_.CreateAllocaBPF(map.type, map.ident + "_val");
    b_.CreateStore(expr, val);
    self_alloca = true;
  }
  b_.CreateMapUpdateElem(ctx_, map, key, val, assignment.loc);
  b_.CreateLifetimeEnd(key);
  if (self_alloca)
    b_.CreateLifetimeEnd(val);
}

void CodegenLLVM::visit(AssignVarStatement &assignment)
{
  Variable &var = *assignment.var;

  auto scoped_del = accept(assignment.expr.get());

  if (variables_.find(var.ident) == variables_.end())
  {
    AllocaInst *val = b_.CreateAllocaBPFInit(var.type, var.ident);
    variables_[var.ident] = val;
  }

  if (needMemcpy(var.type))
  {
    b_.CREATE_MEMCPY(variables_[var.ident], expr_, var.type.size, 1);
  }
  else
  {
    b_.CreateStore(expr_, variables_[var.ident]);
  }
}

void CodegenLLVM::visit(If &if_block)
{
  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *if_true = BasicBlock::Create(module_->getContext(),
                                           "if_body",
                                           parent);
  BasicBlock *if_end = BasicBlock::Create(module_->getContext(),
                                          "if_end",
                                          parent);
  BasicBlock *if_else = nullptr;

  auto scoped_del = accept(if_block.cond.get());
  Value *zero_value = Constant::getNullValue(expr_->getType());
  Value *cond = b_.CreateICmpNE(expr_, zero_value, "true_cond");

  // 3 possible flows:
  //
  // if condition is true
  //   parent -> if_body -> if_end
  //
  // if condition is false, no else
  //   parent -> if_end
  //
  // if condition is false, with else
  //   parent -> if_else -> if_end
  //
  if (if_block.else_stmts)
  {
    // LLVM doesn't accept empty basic block, only create when needed
    if_else = BasicBlock::Create(module_->getContext(), "else_body", parent);
    b_.CreateCondBr(cond, if_true, if_else);
  }
  else
  {
    b_.CreateCondBr(cond, if_true, if_end);
  }

  b_.SetInsertPoint(if_true);
  for (auto &stmt : *if_block.stmts)
    auto scoped_del = accept(stmt.get());

  b_.CreateBr(if_end);

  b_.SetInsertPoint(if_end);

  if (if_block.else_stmts)
  {
    b_.SetInsertPoint(if_else);
    for (auto &stmt : *if_block.else_stmts)
      auto scoped_del = accept(stmt.get());

    b_.CreateBr(if_end);
    b_.SetInsertPoint(if_end);
  }
}

void CodegenLLVM::visit(Unroll &unroll)
{
  for (int i=0; i < unroll.var; i++) {
    for (auto &stmt : *unroll.stmts)
    {
      auto scoped_del = accept(stmt.get());
    }
  }
}

void CodegenLLVM::visit(Jump &jump)
{
  switch (jump.ident)
  {
    case bpftrace::Parser::token::RETURN:
      // return can be used outside of loops
      b_.CreateRet(ConstantInt::get(module_->getContext(), APInt(64, 0)));
      break;
    case bpftrace::Parser::token::BREAK:
      b_.CreateBr(std::get<1>(loops_.back()));
      break;
    case bpftrace::Parser::token::CONTINUE:
      b_.CreateBr(std::get<0>(loops_.back()));
      break;
  }

  // LLVM doesn't like having instructions after an unconditional branch (segv)
  // This can be avoided by putting all instructions in a unreachable basicblock
  // which will be optimize out.
  //
  // e.g. in the case of `while (..) { $i++; break; $i++ }` the ir will be:
  //
  // while_body:
  //   ...
  //   br label %while_end
  //
  // while_end:
  //   ...
  //
  // unreach:
  //   $i++
  //   br label %while_cond
  //

  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *unreach = BasicBlock::Create(module_->getContext(),
                                           "unreach",
                                           parent);
  b_.SetInsertPoint(unreach);
}

void CodegenLLVM::visit(While &while_block)
{
  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *while_cond = BasicBlock::Create(module_->getContext(),
                                              "while_cond",
                                              parent);
  BasicBlock *while_body = BasicBlock::Create(module_->getContext(),
                                              "while_body",
                                              parent);
  BasicBlock *while_end = BasicBlock::Create(module_->getContext(),
                                             "while_end",
                                             parent);

  loops_.push_back(std::make_tuple(while_cond, while_end));

  b_.CreateBr(while_cond);

  b_.SetInsertPoint(while_cond);
  auto scoped_del = accept(while_block.cond.get());
  Value *zero_value = Constant::getNullValue(expr_->getType());
  auto *cond = b_.CreateICmpNE(expr_, zero_value, "true_cond");
  b_.CreateCondBr(cond, while_body, while_end);

  b_.SetInsertPoint(while_body);
  for (auto &stmt : *while_block.stmts)
  {
    auto scoped_del = accept(stmt.get());
  }
  b_.CreateBr(while_cond);

  b_.SetInsertPoint(while_end);
  loops_.pop_back();
}

void CodegenLLVM::visit(Predicate &pred)
{
  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *pred_false_block = BasicBlock::Create(
      module_->getContext(),
      "pred_false",
      parent);
  BasicBlock *pred_true_block = BasicBlock::Create(
      module_->getContext(),
      "pred_true",
      parent);

  auto scoped_del = accept(pred.expr.get());

  // allow unop casts in predicates:
  expr_ = b_.CreateIntCast(expr_, b_.getInt64Ty(), false);

  expr_ = b_.CreateICmpEQ(expr_, b_.getInt64(0), "predcond");

  b_.CreateCondBr(expr_, pred_false_block, pred_true_block);
  b_.SetInsertPoint(pred_false_block);
  b_.CreateRet(ConstantInt::get(module_->getContext(), APInt(64, 0)));

  b_.SetInsertPoint(pred_true_block);
}

void CodegenLLVM::visit(AttachPoint &)
{
  // Empty
}

void CodegenLLVM::generateProbe(Probe &probe,
                                const std::string &full_func_id,
                                const std::string &section_name,
                                FunctionType *func_type,
                                bool expansion)
{
  // tracepoint wildcard expansion, part 3 of 3. Set tracepoint_struct_ for use
  // by args builtin.
  if (probetype(current_attach_point_->provider) == ProbeType::tracepoint)
    tracepoint_struct_ = TracepointFormatParser::get_struct_name(full_func_id);
  int index = getNextIndexForProbe(probe.name());
  if (expansion)
    current_attach_point_->set_index(full_func_id, index);
  else
    probe.set_index(index);
  Function *func = Function::Create(
      func_type, Function::ExternalLinkage, section_name, module_.get());
  func->setSection(getSectionNameForProbe(section_name, index));
  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", func);
  b_.SetInsertPoint(entry);

  // check: do the following 8 lines need to be in the wildcard loop?
  ctx_ = func->arg_begin();
  if (probe.pred)
  {
    auto scoped_del = accept(probe.pred.get());
  }
  variables_.clear();
  for (auto &stmt : *probe.stmts)
  {
    auto scoped_del = accept(stmt.get());
  }
  b_.CreateRet(ConstantInt::get(module_->getContext(), APInt(64, 0)));
}

void CodegenLLVM::visit(Probe &probe)
{
  FunctionType *func_type = FunctionType::get(
      b_.getInt64Ty(),
      {b_.getInt8PtrTy()}, // struct pt_regs *ctx
      false);

  // Probe has at least one attach point (required by the parser)
  auto &attach_point = (*probe.attach_points)[0];

  // All usdt probes need expansion to be able to read arguments
  if (probetype(attach_point->provider) == ProbeType::usdt)
    probe.need_expansion = true;

  current_attach_point_ = attach_point.get();

  /*
   * Most of the time, we can take a probe like kprobe:do_f* and build a
   * single BPF program for that, called "s_kprobe:do_f*", and attach it to
   * each wildcard match. An exception is the "probe" builtin, where we need
   * to build different BPF programs for each wildcard match that cantains an
   * ID for the match. Those programs will be called "s_kprobe:do_fcntl" etc.
   */
  if (probe.need_expansion == false) {
    // build a single BPF program pre-wildcards
    probefull_ = probe.name();
    generateProbe(probe, probefull_, probefull_, func_type, false);
  } else {
    /*
     * Build a separate BPF program for each wildcard match.
     * We begin by saving state that gets changed by the codegen pass, so we
     * can restore it for the next pass (printf_id_, time_id_).
     */
    int starting_printf_id = printf_id_;
    int starting_cat_id = cat_id_;
    int starting_system_id = system_id_;
    int starting_time_id = time_id_;
    int starting_strftime_id = strftime_id_;
    int starting_join_id = join_id_;
    int starting_helper_error_id = b_.helper_error_id_;
    int starting_non_map_print_id = non_map_print_id_;

    auto reset_ids = [&]() {
      printf_id_ = starting_printf_id;
      cat_id_ = starting_cat_id;
      system_id_ = starting_system_id;
      time_id_ = starting_time_id;
      strftime_id_ = starting_strftime_id;
      join_id_ = starting_join_id;
      b_.helper_error_id_ = starting_helper_error_id;
      non_map_print_id_ = starting_non_map_print_id;
    };

    for (auto &attach_point : *probe.attach_points)
    {
      current_attach_point_ = attach_point.get();

      std::set<std::string> matches;
      if (attach_point->provider == "BEGIN" || attach_point->provider == "END") {
        matches.insert(attach_point->provider);
      } else {
        matches = bpftrace_.find_wildcard_matches(*attach_point);
      }

      tracepoint_struct_ = "";
      for (const auto &match : matches)
      {
        reset_ids();

        // USDT probes must specify a target binary path, a provider,
        // and a function name.
        // So we will extract out the path and the provider namespace to get
        // just the function name.
        if (probetype(attach_point->provider) == ProbeType::usdt) {
          std::string func_id = match;
          std::string target = erase_prefix(func_id);
          std::string ns = erase_prefix(func_id);

          std::string orig_target = attach_point->target;
          std::string orig_ns = attach_point->ns;

          // Ensure that the full probe name used is the resolved one for this
          // probe.
          attach_point->target = target;
          attach_point->ns = ns;
          probefull_ = attach_point->name(func_id);

          // Set the probe identifier so that we can read arguments later
          auto usdt = USDTHelper::find(bpftrace_.pid(), target, ns, func_id);
          if (!usdt.has_value())
            throw std::runtime_error("Failed to find usdt probe: " +
                                     probefull_);
          attach_point->usdt = *usdt;

          // A "unique" USDT probe can be present in a binary in multiple
          // locations. One case where this happens is if a function containing
          // a USDT probe is inlined into a caller. So we must generate a new
          // program for each instance. We _must_ regenerate because argument
          // locations may differ between instance locations (eg arg0. may not
          // be found in the same offset from the same register in each
          // location)
          current_usdt_location_index_ = 0;
          for (int i = 0; i < attach_point->usdt.num_locations; ++i)
          {
            reset_ids();

            std::string loc_suffix = "_loc" + std::to_string(i);
            std::string full_func_id = match + loc_suffix;
            std::string section_name = probefull_ + loc_suffix;
            generateProbe(probe, full_func_id, section_name, func_type, true);
            current_usdt_location_index_++;
          }

          // Propagate the originally specified target and namespace in case
          // they contain a wildcard.
          attach_point->target = orig_target;
          attach_point->ns = orig_ns;
        }
        else
        {
          if (attach_point->provider == "BEGIN" ||
              attach_point->provider == "END")
            probefull_ = attach_point->provider;
          else if ((probetype(attach_point->provider) ==
                        ProbeType::tracepoint ||
                    probetype(attach_point->provider) == ProbeType::uprobe ||
                    probetype(attach_point->provider) == ProbeType::uretprobe))
          {
            // Tracepoint and uprobe probes must specify both a target
            // (tracepoint category) and a function name
            std::string func = match;
            std::string category = erase_prefix(func);

            probefull_ = attach_point->name(category, func);
          }
          else
            probefull_ = attach_point->name(match);

          generateProbe(probe, match, probefull_, func_type, true);
        }
      }
    }
  }
  bpftrace_.add_probe(probe);
  current_attach_point_ = nullptr;
}

void CodegenLLVM::visit(Program &program)
{
  for (auto &probe : *program.probes)
    auto scoped_del = accept(probe.get());
}

int CodegenLLVM::getNextIndexForProbe(const std::string &probe_name) {
  if (next_probe_index_.count(probe_name) == 0)
    next_probe_index_[probe_name] = 1;
  int index = next_probe_index_[probe_name];
  next_probe_index_[probe_name] += 1;
  return index;
}

std::string CodegenLLVM::getSectionNameForProbe(const std::string &probe_name, int index) {
  return "s_" + probe_name + "_" + std::to_string(index);
}

AllocaInst *CodegenLLVM::getMapKey(Map &map)
{
  AllocaInst *key;
  if (map.vargs) {
    // A single value as a map key (e.g., @[comm] = 0;)
    if (map.vargs->size() == 1)
    {
      auto &expr = map.vargs->at(0);
      auto scoped_del = accept(expr.get());
      if (shouldBeOnStackAlready(expr->type))
      {
        key = dyn_cast<AllocaInst>(expr_);
        // Call-ee freed
        scoped_del.disarm();
      }
      else
      {
        key = b_.CreateAllocaBPF(expr->type.size, map.ident + "_key");
        b_.CreateStore(
            b_.CreateIntCast(expr_, b_.getInt64Ty(), expr->type.IsSigned()),
            b_.CreatePointerCast(key, expr_->getType()->getPointerTo()));
      }
    }
    else
    {
      // Two or more values as a map key (e.g, @[comm, pid] = 1;)
      size_t size = 0;
      for (auto &expr : *map.vargs)
      {
        size += expr->type.size;
      }
      key = b_.CreateAllocaBPF(size, map.ident + "_key");

      int offset = 0;
      // Construct a map key in the stack
      for (auto &expr : *map.vargs)
      {
        auto scoped_del = accept(expr.get());
        Value *offset_val = b_.CreateGEP(
            key, { b_.getInt64(0), b_.getInt64(offset) });

        if (shouldBeOnStackAlready(expr->type))
          b_.CREATE_MEMCPY(offset_val, expr_, expr->type.size, 1);
        else
        {
          // promote map key to 64-bit:
          b_.CreateStore(
              b_.CreateIntCast(expr_, b_.getInt64Ty(), expr->type.IsSigned()),
              b_.CreatePointerCast(offset_val,
                                   expr_->getType()->getPointerTo()));
        }
        offset += expr->type.size;
      }
    }
  }
  else
  {
    // No map key (e.g., @ = 1;). Use 0 as a key.
    key = b_.CreateAllocaBPF(CreateUInt64(), map.ident + "_key");
    b_.CreateStore(b_.getInt64(0), key);
  }
  return key;
}

AllocaInst *CodegenLLVM::getHistMapKey(Map &map, Value *log2)
{
  AllocaInst *key;
  if (map.vargs) {
    size_t size = 8; // Extra space for the bucket value
    for (auto &expr : *map.vargs)
    {
      size += expr->type.size;
    }
    key = b_.CreateAllocaBPF(size, map.ident + "_key");

    int offset = 0;
    for (auto &expr : *map.vargs)
    {
      auto scoped_del = accept(expr.get());
      Value *offset_val = b_.CreateGEP(key, {b_.getInt64(0), b_.getInt64(offset)});
      if (shouldBeOnStackAlready(expr->type))
        b_.CREATE_MEMCPY(offset_val, expr_, expr->type.size, 1);
      else
        b_.CreateStore(expr_, offset_val);
      offset += expr->type.size;
    }
    Value *offset_val = b_.CreateGEP(key, {b_.getInt64(0), b_.getInt64(offset)});
    b_.CreateStore(log2, offset_val);
  }
  else
  {
    key = b_.CreateAllocaBPF(CreateUInt64(), map.ident + "_key");
    b_.CreateStore(log2, key);
  }
  return key;
}

Value *CodegenLLVM::createLogicalAnd(Binop &binop)
{
  assert(binop.left->type.IsIntTy());
  assert(binop.right->type.IsIntTy());

  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *lhs_true_block = BasicBlock::Create(module_->getContext(), "&&_lhs_true", parent);
  BasicBlock *true_block = BasicBlock::Create(module_->getContext(), "&&_true", parent);
  BasicBlock *false_block = BasicBlock::Create(module_->getContext(), "&&_false", parent);
  BasicBlock *merge_block = BasicBlock::Create(module_->getContext(), "&&_merge", parent);

  Value *result = b_.CreateAllocaBPF(b_.getInt64Ty(), "&&_result");
  Value *lhs;
  auto scoped_del_left = accept(binop.left.get());
  lhs = expr_;
  b_.CreateCondBr(b_.CreateICmpNE(lhs, b_.GetIntSameSize(0, lhs), "lhs_true_cond"),
                  lhs_true_block,
                  false_block);

  b_.SetInsertPoint(lhs_true_block);
  Value *rhs;
  auto scoped_del_right = accept(binop.right.get());
  rhs = expr_;
  b_.CreateCondBr(b_.CreateICmpNE(rhs, b_.GetIntSameSize(0, rhs), "rhs_true_cond"),
                  true_block,
                  false_block);

  b_.SetInsertPoint(true_block);
  b_.CreateStore(b_.getInt64(1), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(false_block);
  b_.CreateStore(b_.getInt64(0), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(merge_block);
  return b_.CreateLoad(result);
}

Value *CodegenLLVM::createLogicalOr(Binop &binop)
{
  assert(binop.left->type.IsIntTy());
  assert(binop.right->type.IsIntTy());

  Function *parent = b_.GetInsertBlock()->getParent();
  BasicBlock *lhs_false_block = BasicBlock::Create(module_->getContext(), "||_lhs_false", parent);
  BasicBlock *false_block = BasicBlock::Create(module_->getContext(), "||_false", parent);
  BasicBlock *true_block = BasicBlock::Create(module_->getContext(), "||_true", parent);
  BasicBlock *merge_block = BasicBlock::Create(module_->getContext(), "||_merge", parent);

  Value *result = b_.CreateAllocaBPF(b_.getInt64Ty(), "||_result");
  Value *lhs;
  auto scoped_del_left = accept(binop.left.get());
  lhs = expr_;
  b_.CreateCondBr(b_.CreateICmpNE(lhs, b_.GetIntSameSize(0, lhs), "lhs_true_cond"),
                  true_block,
                  lhs_false_block);

  b_.SetInsertPoint(lhs_false_block);
  Value *rhs;
  auto scoped_del_right = accept(binop.right.get());
  rhs = expr_;
  b_.CreateCondBr(b_.CreateICmpNE(rhs, b_.GetIntSameSize(0, rhs), "rhs_true_cond"),
                  true_block,
                  false_block);

  b_.SetInsertPoint(false_block);
  b_.CreateStore(b_.getInt64(0), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(true_block);
  b_.CreateStore(b_.getInt64(1), result);
  b_.CreateBr(merge_block);

  b_.SetInsertPoint(merge_block);
  return b_.CreateLoad(result);
}

Function *CodegenLLVM::createLog2Function()
{
  auto ip = b_.saveIP();
  // log2() returns a bucket index for the given value. Index 0 is for
  // values less than 0, index 1 is for 0, and indexes 2 onwards is the
  // power-of-2 histogram index.
  //
  // log2(int n)
  // {
  //   int result = 0;
  //   int shift;
  //   if (n < 0) return result;
  //   result++;
  //   if (n == 0) return result;
  //   result++;
  //   for (int i = 4; i >= 0; i--)
  //   {
  //     shift = (v >= (1<<(1<<i))) << i;
  //     n >> = shift;
  //     result += shift;
  //   }
  //   return result;
  // }

  FunctionType *log2_func_type = FunctionType::get(b_.getInt64Ty(), {b_.getInt64Ty()}, false);
  Function *log2_func = Function::Create(log2_func_type, Function::InternalLinkage, "log2", module_.get());
  log2_func->addFnAttr(Attribute::AlwaysInline);
  log2_func->setSection("helpers");
  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", log2_func);
  b_.SetInsertPoint(entry);

  // setup n and result registers
  Value *arg = log2_func->arg_begin();
  Value *n_alloc = b_.CreateAllocaBPF(CreateUInt64());
  b_.CreateStore(arg, n_alloc);
  Value *result = b_.CreateAllocaBPF(CreateUInt64());
  b_.CreateStore(b_.getInt64(0), result);

  // test for less than zero
  BasicBlock *is_less_than_zero = BasicBlock::Create(module_->getContext(), "hist.is_less_than_zero", log2_func);
  BasicBlock *is_not_less_than_zero = BasicBlock::Create(module_->getContext(), "hist.is_not_less_than_zero", log2_func);
  b_.CreateCondBr(b_.CreateICmpSLT(b_.CreateLoad(n_alloc), b_.getInt64(0)),
                  is_less_than_zero,
                  is_not_less_than_zero);
  b_.SetInsertPoint(is_less_than_zero);
  b_.CreateRet(b_.CreateLoad(result));
  b_.SetInsertPoint(is_not_less_than_zero);

  // test for equal to zero
  BasicBlock *is_zero = BasicBlock::Create(module_->getContext(), "hist.is_zero", log2_func);
  BasicBlock *is_not_zero = BasicBlock::Create(module_->getContext(), "hist.is_not_zero", log2_func);
  b_.CreateCondBr(b_.CreateICmpEQ(b_.CreateLoad(n_alloc), b_.getInt64(0)),
                  is_zero,
                  is_not_zero);
  b_.SetInsertPoint(is_zero);
  b_.CreateStore(b_.getInt64(1), result);
  b_.CreateRet(b_.CreateLoad(result));
  b_.SetInsertPoint(is_not_zero);

  // power-of-2 index, offset by +2
  b_.CreateStore(b_.getInt64(2), result);
  for (int i = 4; i >= 0; i--)
  {
    Value *n = b_.CreateLoad(n_alloc);
    Value *shift = b_.CreateShl(b_.CreateIntCast(b_.CreateICmpSGE(b_.CreateIntCast(n, b_.getInt64Ty(), false), b_.getInt64(1 << (1<<i))), b_.getInt64Ty(), false), i);
    b_.CreateStore(b_.CreateLShr(n, shift), n_alloc);
    b_.CreateStore(b_.CreateAdd(b_.CreateLoad(result), shift), result);
  }
  b_.CreateRet(b_.CreateLoad(result));
  b_.restoreIP(ip);
  return module_->getFunction("log2");
}

Function *CodegenLLVM::createLinearFunction()
{
  auto ip = b_.saveIP();
  // lhist() returns a bucket index for the given value. The first and last
  //   bucket indexes are special: they are 0 for the less-than-range
  //   bucket, and index max_bucket+2 for the greater-than-range bucket.
  //   Indexes 1 to max_bucket+1 span the buckets in the range.
  //
  // int lhist(int value, int min, int max, int step)
  // {
  //   int result;
  //
  //   if (value < min)
  //     return 0;
  //   if (value > max)
  //     return 1 + (max - min) / step;
  //   result = 1 + (value - min) / step;
  //
  //   return result;
  // }

  // inlined function initialization
  FunctionType *linear_func_type = FunctionType::get(b_.getInt64Ty(), {b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty(), b_.getInt64Ty()}, false);
  Function *linear_func = Function::Create(linear_func_type, Function::InternalLinkage, "linear", module_.get());
  linear_func->addFnAttr(Attribute::AlwaysInline);
  linear_func->setSection("helpers");
  BasicBlock *entry = BasicBlock::Create(module_->getContext(), "entry", linear_func);
  b_.SetInsertPoint(entry);

  // pull in arguments
  Value *value_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *min_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *max_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *step_alloc = b_.CreateAllocaBPF(CreateUInt64());
  Value *result_alloc = b_.CreateAllocaBPF(CreateUInt64());

  b_.CreateStore(linear_func->arg_begin() + 0, value_alloc);
  b_.CreateStore(linear_func->arg_begin() + 1, min_alloc);
  b_.CreateStore(linear_func->arg_begin() + 2, max_alloc);
  b_.CreateStore(linear_func->arg_begin() + 3, step_alloc);

  Value *cmp = nullptr;

  // algorithm
  {
    Value *min = b_.CreateLoad(min_alloc);
    Value *val = b_.CreateLoad(value_alloc);
    cmp = b_.CreateICmpSLT(val, min);
  }
  BasicBlock *lt_min = BasicBlock::Create(module_->getContext(), "lhist.lt_min", linear_func);
  BasicBlock *ge_min = BasicBlock::Create(module_->getContext(), "lhist.ge_min", linear_func);
  b_.CreateCondBr(cmp, lt_min, ge_min);

  b_.SetInsertPoint(lt_min);
  b_.CreateRet(b_.getInt64(0));

  b_.SetInsertPoint(ge_min);
  {
    Value *max = b_.CreateLoad(max_alloc);
    Value *val = b_.CreateLoad(value_alloc);
    cmp = b_.CreateICmpSGT(val, max);
  }
  BasicBlock *le_max = BasicBlock::Create(module_->getContext(), "lhist.le_max", linear_func);
  BasicBlock *gt_max = BasicBlock::Create(module_->getContext(), "lhist.gt_max", linear_func);
  b_.CreateCondBr(cmp, gt_max, le_max);

  b_.SetInsertPoint(gt_max);
  {
    Value *step = b_.CreateLoad(step_alloc);
    Value *min = b_.CreateLoad(min_alloc);
    Value *max = b_.CreateLoad(max_alloc);
    Value *div = b_.CreateUDiv(b_.CreateSub(max, min), step);
    b_.CreateStore(b_.CreateAdd(div, b_.getInt64(1)), result_alloc);
    b_.CreateRet(b_.CreateLoad(result_alloc));
  }

  b_.SetInsertPoint(le_max);
  {
    Value *step = b_.CreateLoad(step_alloc);
    Value *min = b_.CreateLoad(min_alloc);
    Value *val = b_.CreateLoad(value_alloc);
    Value *div3 = b_.CreateUDiv(b_.CreateSub(val, min), step);
    b_.CreateStore(b_.CreateAdd(div3, b_.getInt64(1)), result_alloc);
    b_.CreateRet(b_.CreateLoad(result_alloc));
  }

  b_.restoreIP(ip);
  return module_->getFunction("linear");
}

void CodegenLLVM::createFormatStringCall(Call &call, int &id, CallArgs &call_args,
                                         const std::string &call_name, AsyncAction async_action)
{
  /*
   * perf event output has: uint64_t id, vargs
   * The id maps to bpftrace_.*_args_, and is a way to define the
   * types and offsets of each of the arguments, and share that between BPF and
   * user-space for printing.
   */
  std::vector<llvm::Type *> elements = { b_.getInt64Ty() }; // ID

  auto &args = std::get<1>(call_args.at(id));
  for (Field &arg : args)
  {
    llvm::Type *ty = b_.GetType(arg.type);
    elements.push_back(ty);
  }
  StructType *fmt_struct = StructType::create(elements, call_name + "_t", false);
  int struct_size = layout_.getTypeAllocSize(fmt_struct);

  auto *struct_layout = layout_.getStructLayout(fmt_struct);
  for (size_t i=0; i<args.size(); i++)
  {
    Field &arg = args[i];
    arg.offset = struct_layout->getElementOffset(i+1); // +1 for the id field
  }

  AllocaInst *fmt_args = b_.CreateAllocaBPF(fmt_struct, call_name + "_args");
  // as the struct is not packed we need to memset it.
  b_.CREATE_MEMSET(fmt_args, b_.getInt8(0), struct_size, 1);

  Value *id_offset = b_.CreateGEP(fmt_args, {b_.getInt32(0), b_.getInt32(0)});
  b_.CreateStore(b_.getInt64(id + asyncactionint(async_action)), id_offset);

  for (size_t i=1; i<call.vargs->size(); i++)
  {
    Expression &arg = *call.vargs->at(i);
    auto scoped_del = accept(&arg);
    Value *offset = b_.CreateGEP(fmt_args, {b_.getInt32(0), b_.getInt32(i)});
    if (needMemcpy(arg.type))
      b_.CREATE_MEMCPY(offset, expr_, arg.type.size, 1);
    else
      b_.CreateStore(expr_, offset);
  }

  id++;
  b_.CreatePerfEventOutput(ctx_, fmt_args, struct_size);
  b_.CreateLifetimeEnd(fmt_args);
  expr_ = nullptr;
}

void CodegenLLVM::createPrintMapCall(Call &call)
{
  auto elements = AsyncEvent::Print().asLLVMType(b_);
  StructType *print_struct = b_.GetStructType(call.func + "_t", elements, true);

  auto &arg = *call.vargs->at(0);
  auto &map = static_cast<Map &>(arg);

  AllocaInst *buf = b_.CreateAllocaBPF(print_struct,
                                       call.func + "_" + map.ident);

  // store asyncactionid:
  b_.CreateStore(b_.getInt64(asyncactionint(AsyncAction::print)),
                 b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) }));

  auto id = bpftrace_.maps[map.ident].value()->id;
  auto *ident_ptr = b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(1) });
  b_.CreateStore(b_.GetIntSameSize(id, elements.at(1)), ident_ptr);

  // top, div
  // first loops sets the arguments as passed by user. The second one zeros
  // the rest
  size_t arg_idx = 1;
  for (; arg_idx < call.vargs->size(); arg_idx++)
  {
    auto scoped_del = accept(call.vargs->at(arg_idx).get());

    b_.CreateStore(b_.CreateIntCast(expr_, elements.at(arg_idx), false),
                   b_.CreateGEP(buf,
                                { b_.getInt64(0), b_.getInt32(arg_idx + 1) }));
  }

  for (; arg_idx < 3; arg_idx++)
  {
    b_.CreateStore(b_.GetIntSameSize(0, elements.at(arg_idx)),
                   b_.CreateGEP(buf,
                                { b_.getInt64(0), b_.getInt32(arg_idx + 1) }));
  }

  b_.CreatePerfEventOutput(ctx_, buf, getStructSize(print_struct));
  b_.CreateLifetimeEnd(buf);
  expr_ = nullptr;
}

void CodegenLLVM::createPrintNonMapCall(Call &call, int &id)
{
  auto &arg = *call.vargs->at(0);
  auto scoped_del = accept(&arg);

  auto elements = AsyncEvent::PrintNonMap().asLLVMType(b_, arg.type.size);
  std::ostringstream struct_name;
  struct_name << call.func << "_" << arg.type.type << "_" << arg.type.size
              << "_t";
  StructType *print_struct = b_.GetStructType(struct_name.str(),
                                              elements,
                                              true);
  AllocaInst *buf = b_.CreateAllocaBPF(print_struct, struct_name.str());
  size_t struct_size = layout_.getTypeAllocSize(print_struct);

  // Store asyncactionid:
  b_.CreateStore(b_.getInt64(asyncactionint(AsyncAction::print_non_map)),
                 b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(0) }));

  // Store print id
  b_.CreateStore(b_.getInt64(id),
                 b_.CreateGEP(buf, { b_.getInt64(0), b_.getInt32(1) }));

  // Store content
  Value *content_offset = b_.CreateGEP(buf, { b_.getInt32(0), b_.getInt32(2) });
  b_.CREATE_MEMSET(content_offset, b_.getInt8(0), arg.type.size, 1);
  if (needMemcpy(arg.type))
    b_.CREATE_MEMCPY(content_offset, expr_, arg.type.size, 1);
  else
  {
    auto ptr = b_.CreatePointerCast(content_offset,
                                    expr_->getType()->getPointerTo());
    b_.CreateStore(expr_, ptr);
  }

  id++;
  b_.CreatePerfEventOutput(ctx_, buf, struct_size);
  b_.CreateLifetimeEnd(buf);
  expr_ = nullptr;
}

void CodegenLLVM::generate_ir()
{
  assert(state_ == State::INIT);
  auto scoped_del = accept(root_);
  state_ = State::IR;
}

void CodegenLLVM::emit_elf(const std::string &filename)
{
  assert(state_ == State::OPT);
  legacy::PassManager PM;

#if LLVM_VERSION_MAJOR >= 10
  auto type = llvm::CGFT_ObjectFile;
#else
  auto type = llvm::TargetMachine::CGFT_ObjectFile;
#endif

#if LLVM_VERSION_MAJOR >= 7
  std::error_code err;
  raw_fd_ostream out(filename, err);

  if (err)
    throw std::system_error(err.value(),
                            std::generic_category(),
                            "Failed to open: " + filename);
  if (TM_->addPassesToEmitFile(PM, out, nullptr, type))
    throw std::runtime_error("Cannot emit a file of this type");
  PM.run(*module_.get());

  return;

#else
  std::ofstream file(filename);
  if (!file.is_open())
    throw std::system_error(errno,
                            std::generic_category(),
                            "Failed to open: " + filename);
  std::unique_ptr<SmallVectorImpl<char>> buf(new SmallVector<char, 0>());
  raw_svector_ostream out(*buf);

  if (TM_->addPassesToEmitFile(PM, out, type, true, nullptr))
    throw std::runtime_error("Cannot emit a file of this type");

  file.write(buf->data(), buf->size_in_bytes());
#endif
}

void CodegenLLVM::optimize()
{
  assert(state_ == State::IR);
  PassManagerBuilder PMB;
  PMB.OptLevel = 3;
  legacy::PassManager PM;
  PM.add(createFunctionInliningPass());
  /*
   * llvm < 4.0 needs
   * PM.add(createAlwaysInlinerPass());
   * llvm >= 4.0 needs
   * PM.add(createAlwaysInlinerLegacyPass());
   * use below 'stable' workaround
   */
  LLVMAddAlwaysInlinerPass(reinterpret_cast<LLVMPassManagerRef>(&PM));
  PMB.populateModulePassManager(PM);

  PM.run(*module_.get());
  state_ = State::OPT;
}

std::unique_ptr<BpfOrc> CodegenLLVM::emit(void)
{
  assert(state_ == State::OPT);
  orc_->compileModule(move(module_));
  state_ = State::DONE;
  return std::move(orc_);
}

std::unique_ptr<BpfOrc> CodegenLLVM::compile(void)
{
  generate_ir();
  optimize();
  return emit();
}

void CodegenLLVM::DumpIR(void)
{
  DumpIR(std::cout);
}

void CodegenLLVM::DumpIR(std::ostream &out)
{
  assert(module_.get() != nullptr);
  raw_os_ostream os(out);
  module_->print(os, nullptr, false, true);
}

CodegenLLVM::ScopedExprDeleter CodegenLLVM::accept(Node *node)
{
  expr_deleter_ = nullptr;
  node->accept(*this);
  auto deleter = std::move(expr_deleter_);
  expr_deleter_ = nullptr;
  return ScopedExprDeleter(deleter);
}

} // namespace ast
} // namespace bpftrace
