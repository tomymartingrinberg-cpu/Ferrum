#include "ferrum/Codegen.h"

#include <llvm/IR/Verifier.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>

#include <sstream>
#include <stdexcept>

namespace ferrum {

// ─── Constructor ──────────────────────────────────────────────────────────────

Codegen::Codegen(const std::string& moduleName, TypeChecker& tc_)
    : mod(std::make_unique<llvm::Module>(moduleName, ctx))
    , builder(std::make_unique<llvm::IRBuilder<>>(ctx))
    , tc(tc_)
{}

// ─── Error ────────────────────────────────────────────────────────────────────

void Codegen::addError(int line, const std::string& msg) {
    errors.push_back({line, msg});
}

// ─── Type mappings ────────────────────────────────────────────────────────────

llvm::Type* Codegen::toLLVM(const TypeRef& ref) {
    switch (ref.kind) {
    case TypeRef::Kind::Named:
        if (ref.name == "int")   return i32Ty();
        if (ref.name == "float") return f64Ty();
        if (ref.name == "char")  return i8Ty();
        if (ref.name == "bool")  return i1Ty();
        if (ref.name == "void")  return voidTy();
        return i64Ty(); // fallback for unknown/generic types
    case TypeRef::Kind::Pointer:
    case TypeRef::Kind::Borrow:
    case TypeRef::Kind::BorrowMut:
    case TypeRef::Kind::Array:
        return ptrTy();
    }
    return i64Ty();
}

llvm::Type* Codegen::toLLVM(const FerType& ft) {
    switch (ft.kind) {
    case FerType::Kind::Void:      return voidTy();
    case FerType::Kind::Int:       return i32Ty();
    case FerType::Kind::Float:     return f64Ty();
    case FerType::Kind::Char:      return i8Ty();
    case FerType::Kind::Bool:      return i1Ty();
    case FerType::Kind::Pointer:
    case FerType::Kind::Borrow:
    case FerType::Kind::BorrowMut: return ptrTy();
    case FerType::Kind::Generic:   return i64Ty(); // monomorphization not done yet
    case FerType::Kind::Struct:    return i64Ty(); // struct layout not implemented yet
    case FerType::Kind::Function:  return ptrTy();
    }
    return i64Ty();
}

uint64_t Codegen::sizeOf(llvm::Type* ty) {
    if (ty == i8Ty())  return 1;
    if (ty == i1Ty())  return 1;
    if (ty == i32Ty()) return 4;
    if (ty == f64Ty()) return 8;
    if (ty == i64Ty()) return 8;
    if (ty == ptrTy()) return 8;
    return 8;
}

// ─── Scope management ─────────────────────────────────────────────────────────

void Codegen::pushScope() { scopes.push_back({}); }

void Codegen::popScope(llvm::Function* func) {
    if (scopes.empty()) return;
    auto& scope = scopes.back();
    if (!scope.cleanupDone) {
        // Emit free() for heap-owned, non-moved variables
        for (auto& [name, info] : scope.vars) {
            if (info.isHeap && !info.isMoved && info.addr) {
                auto* heapPtr = builder->CreateLoad(ptrTy(), info.addr, name + "_free");
                builder->CreateCall(getOrDeclareFree(), {heapPtr});
            }
        }
    }
    scopes.pop_back();
}

void Codegen::declareVar(const std::string& name, VarInfo info) {
    if (!scopes.empty()) scopes.back().vars[name] = std::move(info);
}

Codegen::VarInfo* Codegen::lookupVar(const std::string& name) {
    for (int i = (int)scopes.size()-1; i >= 0; --i) {
        auto it = scopes[i].vars.find(name);
        if (it != scopes[i].vars.end()) return &it->second;
    }
    return nullptr;
}

llvm::Value* Codegen::getVarAddr(const std::string& name) {
    auto* info = lookupVar(name);
    return info ? info->addr : nullptr;
}

void Codegen::emitCleanupAll(llvm::Function* func) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (!it->cleanupDone) {
            for (auto& [name, info] : it->vars) {
                if (info.isHeap && !info.isMoved && info.addr) {
                    auto* heapPtr = builder->CreateLoad(ptrTy(), info.addr, name + "_cleanup");
                    builder->CreateCall(getOrDeclareFree(), {heapPtr});
                }
            }
            it->cleanupDone = true;
        }
    }
}

// ─── Extern function declarations ─────────────────────────────────────────────

llvm::Function* Codegen::getOrDeclareFunction(const std::string& name,
                                               llvm::FunctionType* ftype,
                                               bool /*variadic*/) {
    if (auto* f = mod->getFunction(name)) return f;
    auto* f = llvm::Function::Create(ftype, llvm::Function::ExternalLinkage, name, mod.get());
    functions[name] = f;
    return f;
}

llvm::Function* Codegen::getOrDeclarePrintf() {
    if (auto* f = mod->getFunction("printf")) return f;
    auto* ftype = llvm::FunctionType::get(i32Ty(), {ptrTy()}, /*variadic=*/true);
    return getOrDeclareFunction("printf", ftype, true);
}

llvm::Function* Codegen::getOrDeclareMalloc() {
    if (auto* f = mod->getFunction("malloc")) return f;
    auto* ftype = llvm::FunctionType::get(ptrTy(), {i64Ty()}, false);
    return getOrDeclareFunction("malloc", ftype);
}

llvm::Function* Codegen::getOrDeclareFree() {
    if (auto* f = mod->getFunction("free")) return f;
    auto* ftype = llvm::FunctionType::get(voidTy(), {ptrTy()}, false);
    return getOrDeclareFunction("free", ftype);
}

// ─── IR output ────────────────────────────────────────────────────────────────

std::string Codegen::getIR() const {
    std::string ir;
    llvm::raw_string_ostream os(ir);
    mod->print(os, nullptr);
    return ir;
}

bool Codegen::writeIR(const std::string& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
    if (ec) return false;
    mod->print(out, nullptr);
    return true;
}

// ─── Declaration codegen ──────────────────────────────────────────────────────

void Codegen::genDecl(const Decl& decl) {
    switch (decl.kind) {
    case Decl::Kind::Import:
        importedHeaders.push_back(decl.importPath);
        // Pre-declare common functions for known C headers
        if (decl.importPath == "stdio.h" || decl.importPath == "cstdio")
            getOrDeclarePrintf();
        if (decl.importPath == "stdlib.h" || decl.importPath == "cstdlib") {
            getOrDeclareMalloc();
            getOrDeclareFree();
        }
        break;

    case Decl::Kind::ExternBlock:
        for (auto& d : decl.externDecls) genDecl(*d);
        break;

    case Decl::Kind::Struct:
        // Struct layout codegen is a future enhancement
        break;

    case Decl::Kind::Function: {
        if (decl.typeParams.size() > 0) {
            // Generic functions: skip codegen (type-checked but not monomorphized yet)
            addError(decl.line, "generic function '" + decl.funcName +
                "' — monomorphization codegen not yet implemented, skipping");
            break;
        }

        // Build LLVM function type
        std::vector<llvm::Type*> paramTys;
        for (auto& p : decl.params) paramTys.push_back(toLLVM(p.type));

        llvm::Type* retTy = decl.returnType ? toLLVM(*decl.returnType) : voidTy();
        // main() must return i32 for OS compatibility
        if (decl.funcName == "main") retTy = i32Ty();

        auto* ftype = llvm::FunctionType::get(retTy, paramTys, false);
        auto* func  = llvm::Function::Create(ftype, llvm::Function::ExternalLinkage,
                                              decl.funcName, mod.get());
        functions[decl.funcName] = func;

        if (!decl.funcBody) break; // extern declaration only

        // Entry basic block
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", func);
        builder->SetInsertPoint(entry);

        pushScope();

        // Alloca for each parameter and store the incoming arg value
        size_t argIdx = 0;
        for (auto& arg : func->args()) {
            auto& p = decl.params[argIdx++];
            auto* ty = toLLVM(p.type);
            auto* addr = builder->CreateAlloca(ty, nullptr, p.name);
            builder->CreateStore(&arg, addr);
            declareVar(p.name, {addr, ty, false, false});
        }

        genStmt(*decl.funcBody, func);

        // If the function didn't end with a return, add a default one
        if (!builder->GetInsertBlock()->getTerminator()) {
            emitCleanupAll(func);
            if (retTy == voidTy())
                builder->CreateRetVoid();
            else
                builder->CreateRet(llvm::Constant::getNullValue(retTy));
        }

        popScope(func);

        // Verify
        std::string verifyErr;
        llvm::raw_string_ostream verifyStream(verifyErr);
        if (llvm::verifyFunction(*func, &verifyStream)) {
            addError(decl.line, "LLVM verify error in '" + decl.funcName + "': " + verifyErr);
        }
        break;
    }
    }
}

// ─── Statement codegen ────────────────────────────────────────────────────────

void Codegen::genStmt(const Stmt& stmt, llvm::Function* func) {
    // After a terminator is emitted, don't try to emit more instructions
    if (builder->GetInsertBlock() && builder->GetInsertBlock()->getTerminator())
        return;

    switch (stmt.kind) {
    case Stmt::Kind::Block: {
        pushScope();
        for (auto& s : stmt.stmts) genStmt(*s, func);
        popScope(func);
        break;
    }

    case Stmt::Kind::VarDecl: {
        // Determine LLVM type for the variable
        llvm::Type* valTy = i32Ty(); // default
        if (stmt.varType) {
            valTy = toLLVM(*stmt.varType);
        } else if (stmt.varInit) {
            auto ft = tc.getExprType(stmt.varInit.get());
            if (ft) valTy = toLLVM(*ft);
        }

        bool isHeap = false;
        bool isMoved = false;

        // Alloca for the variable slot
        // For pointer types (borrow, owning ptr), the slot holds a ptr value
        llvm::Value* addr = builder->CreateAlloca(valTy, nullptr, stmt.varName);

        if (stmt.varInit) {
            if (stmt.varInit->kind == Expr::Kind::New) {
                isHeap = true;
                // Allocate on heap and store initializer
                llvm::Type* innerTy = i32Ty();
                if (stmt.varInit->typeArg) innerTy = toLLVM(*stmt.varInit->typeArg);
                uint64_t sz = sizeOf(innerTy);
                auto* heapPtr = builder->CreateCall(getOrDeclareMalloc(),
                    {llvm::ConstantInt::get(i64Ty(), sz)}, stmt.varName + "_heap");
                if (stmt.varInit->inner) {
                    auto* initVal = genExpr(*stmt.varInit->inner, func);
                    builder->CreateStore(initVal, heapPtr);
                }
                // Store the heap pointer into the alloca
                builder->CreateStore(heapPtr, addr);
            } else if (stmt.varInit->kind == Expr::Kind::Move) {
                // Transfer ownership: load the ptr from source, store into dest
                if (stmt.varInit->inner && stmt.varInit->inner->kind == Expr::Kind::Ident) {
                    const auto& srcName = stmt.varInit->inner->name;
                    auto* srcInfo = lookupVar(srcName);
                    if (srcInfo) {
                        // Load the value from source's slot
                        auto* val = builder->CreateLoad(srcInfo->type, srcInfo->addr, srcName + "_moved");
                        builder->CreateStore(val, addr);
                        // If source was heap-owned, transfer that status
                        if (srcInfo->isHeap) {
                            isHeap = true;
                            srcInfo->isMoved = true; // source no longer owns
                        }
                    }
                }
            } else if (stmt.varInit->kind == Expr::Kind::Borrow ||
                       stmt.varInit->kind == Expr::Kind::BorrowMut) {
                // &x → store address of x into the borrow slot (which is a ptr type)
                if (stmt.varInit->inner && stmt.varInit->inner->kind == Expr::Kind::Ident) {
                    auto* srcAddr = getVarAddr(stmt.varInit->inner->name);
                    if (srcAddr) builder->CreateStore(srcAddr, addr);
                } else if (stmt.varInit->inner) {
                    auto* val = genExpr(*stmt.varInit->inner, func);
                    builder->CreateStore(val, addr);
                }
            } else {
                auto* val = genExpr(*stmt.varInit, func);
                if (val) builder->CreateStore(val, addr);
            }
        }

        declareVar(stmt.varName, {addr, valTy, isHeap, isMoved});
        break;
    }

    case Stmt::Kind::Expr:
        if (stmt.expr) genExpr(*stmt.expr, func);
        break;

    case Stmt::Kind::Return: {
        llvm::Value* retVal = nullptr;
        if (stmt.expr) retVal = genExpr(*stmt.expr, func);
        // If returning a heap-owned variable, transfer ownership OUT before
        // emitCleanupAll runs — otherwise the variable gets free()'d and
        // the returned pointer is a dangling pointer (use-after-free).
        if (stmt.expr && stmt.expr->kind == Expr::Kind::Ident) {
            auto* info = lookupVar(stmt.expr->name);
            if (info && info->isHeap && !info->isMoved)
                info->isMoved = true;
        }
        emitCleanupAll(func);
        if (retVal)
            builder->CreateRet(retVal);
        else
            builder->CreateRetVoid();
        break;
    }

    case Stmt::Kind::If: {
        llvm::Value* cond = stmt.condition ? genExpr(*stmt.condition, func) : nullptr;
        if (!cond) cond = llvm::ConstantInt::getFalse(ctx);

        // Ensure condition is i1
        if (cond->getType() != i1Ty())
            cond = builder->CreateICmpNE(cond, llvm::Constant::getNullValue(cond->getType()), "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(ctx, "then", func);
        auto* mergeBB = llvm::BasicBlock::Create(ctx, "ifmerge", func);
        llvm::BasicBlock* elseBB = stmt.elseBranch ? llvm::BasicBlock::Create(ctx, "else", func) : mergeBB;

        builder->CreateCondBr(cond, thenBB, elseBB);

        // Then branch
        builder->SetInsertPoint(thenBB);
        if (stmt.thenBranch) genStmt(*stmt.thenBranch, func);
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(mergeBB);

        // Else branch
        if (stmt.elseBranch) {
            builder->SetInsertPoint(elseBB);
            genStmt(*stmt.elseBranch, func);
            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(mergeBB);
        }

        builder->SetInsertPoint(mergeBB);
        break;
    }

    case Stmt::Kind::While: {
        auto* condBB  = llvm::BasicBlock::Create(ctx, "while.cond", func);
        auto* bodyBB  = llvm::BasicBlock::Create(ctx, "while.body", func);
        auto* afterBB = llvm::BasicBlock::Create(ctx, "while.after", func);

        builder->CreateBr(condBB);
        builder->SetInsertPoint(condBB);

        llvm::Value* cond = stmt.condition ? genExpr(*stmt.condition, func) : nullptr;
        if (!cond) cond = llvm::ConstantInt::getFalse(ctx);
        if (cond->getType() != i1Ty())
            cond = builder->CreateICmpNE(cond, llvm::Constant::getNullValue(cond->getType()), "whilecond");

        builder->CreateCondBr(cond, bodyBB, afterBB);

        builder->SetInsertPoint(bodyBB);
        if (stmt.body) genStmt(*stmt.body, func);
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(condBB);

        builder->SetInsertPoint(afterBB);
        break;
    }

    case Stmt::Kind::For: {
        // for (init; cond; inc) body
        pushScope();
        if (stmt.init) genStmt(*stmt.init, func);

        auto* condBB  = llvm::BasicBlock::Create(ctx, "for.cond", func);
        auto* bodyBB  = llvm::BasicBlock::Create(ctx, "for.body", func);
        auto* incBB   = llvm::BasicBlock::Create(ctx, "for.inc",  func);
        auto* afterBB = llvm::BasicBlock::Create(ctx, "for.after", func);

        builder->CreateBr(condBB);
        builder->SetInsertPoint(condBB);

        llvm::Value* cond = stmt.condition ? genExpr(*stmt.condition, func) : nullptr;
        if (!cond) cond = llvm::ConstantInt::getTrue(ctx);
        if (cond->getType() != i1Ty())
            cond = builder->CreateICmpNE(cond, llvm::Constant::getNullValue(cond->getType()), "forcond");
        builder->CreateCondBr(cond, bodyBB, afterBB);

        builder->SetInsertPoint(bodyBB);
        if (stmt.body) genStmt(*stmt.body, func);
        if (!builder->GetInsertBlock()->getTerminator())
            builder->CreateBr(incBB);

        builder->SetInsertPoint(incBB);
        if (stmt.increment) genExpr(*stmt.increment, func);
        builder->CreateBr(condBB);

        builder->SetInsertPoint(afterBB);
        popScope(func);
        break;
    }

    case Stmt::Kind::Unsafe:
        // Unsafe blocks: same codegen, safety is enforced at the borrow-check stage
        for (auto& s : stmt.stmts) genStmt(*s, func);
        break;

    default: break;
    }
}

// ─── Expression codegen ───────────────────────────────────────────────────────

llvm::Value* Codegen::genExpr(const Expr& expr, llvm::Function* func) {
    switch (expr.kind) {
    case Expr::Kind::IntLit:
        return llvm::ConstantInt::get(i32Ty(), expr.intVal);

    case Expr::Kind::FloatLit:
        return llvm::ConstantFP::get(f64Ty(), expr.floatVal);

    case Expr::Kind::BoolLit:
        return llvm::ConstantInt::get(i1Ty(), expr.boolVal ? 1 : 0);

    case Expr::Kind::NullLit:
        return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy()));

    case Expr::Kind::StringLit:
        return builder->CreateGlobalStringPtr(expr.strVal);

    case Expr::Kind::Ident: {
        auto* info = lookupVar(expr.name);
        if (!info || !info->addr) {
            addError(expr.line, "unknown variable '" + expr.name + "'");
            return llvm::Constant::getNullValue(i32Ty());
        }
        return builder->CreateLoad(info->type, info->addr, expr.name);
    }

    case Expr::Kind::BinOp: {
        auto* lv = expr.lhs ? genExpr(*expr.lhs, func) : nullptr;
        auto* rv = expr.rhs ? genExpr(*expr.rhs, func) : nullptr;
        if (!lv || !rv) return llvm::Constant::getNullValue(i32Ty());

        // Promote to same type if needed
        bool isFloat = lv->getType()->isDoubleTy() || rv->getType()->isDoubleTy();
        if (isFloat) {
            if (!lv->getType()->isDoubleTy()) lv = builder->CreateSIToFP(lv, f64Ty());
            if (!rv->getType()->isDoubleTy()) rv = builder->CreateSIToFP(rv, f64Ty());
        }

        const auto& op = expr.op;
        if (op == "+")  return isFloat ? builder->CreateFAdd(lv, rv) : builder->CreateAdd(lv, rv);
        if (op == "-")  return isFloat ? builder->CreateFSub(lv, rv) : builder->CreateSub(lv, rv);
        if (op == "*")  return isFloat ? builder->CreateFMul(lv, rv) : builder->CreateMul(lv, rv);
        if (op == "/")  return isFloat ? builder->CreateFDiv(lv, rv) : builder->CreateSDiv(lv, rv);
        if (op == "%")  return builder->CreateSRem(lv, rv);
        if (op == "==") return isFloat ? builder->CreateFCmpOEQ(lv, rv) : builder->CreateICmpEQ(lv, rv);
        if (op == "!=") return isFloat ? builder->CreateFCmpONE(lv, rv) : builder->CreateICmpNE(lv, rv);
        if (op == "<")  return isFloat ? builder->CreateFCmpOLT(lv, rv) : builder->CreateICmpSLT(lv, rv);
        if (op == ">")  return isFloat ? builder->CreateFCmpOGT(lv, rv) : builder->CreateICmpSGT(lv, rv);
        if (op == "<=") return isFloat ? builder->CreateFCmpOLE(lv, rv) : builder->CreateICmpSLE(lv, rv);
        if (op == ">=") return isFloat ? builder->CreateFCmpOGE(lv, rv) : builder->CreateICmpSGE(lv, rv);
        if (op == "&&") return builder->CreateAnd(lv, rv);
        if (op == "||") return builder->CreateOr(lv, rv);
        if (op == "&")  return builder->CreateAnd(lv, rv);
        if (op == "|")  return builder->CreateOr(lv, rv);
        if (op == "^")  return builder->CreateXor(lv, rv);
        return llvm::Constant::getNullValue(i32Ty());
    }

    case Expr::Kind::UnaryOp: {
        if (expr.op == "&") {
            // Address-of: return pointer to the variable
            if (expr.inner && expr.inner->kind == Expr::Kind::Ident)
                return getVarAddr(expr.inner->name);
        }
        auto* val = expr.inner ? genExpr(*expr.inner, func) : nullptr;
        if (!val) return llvm::Constant::getNullValue(i32Ty());
        if (expr.op == "-") return val->getType()->isDoubleTy()
            ? builder->CreateFNeg(val) : builder->CreateNeg(val);
        if (expr.op == "!") return builder->CreateNot(val);
        if (expr.op == "*") {
            // Dereference: load through the pointer
            // val is a ptr value; we need to know what type to load
            auto ft = tc.getExprType(&expr);
            llvm::Type* innerTy = ft ? toLLVM(*ft) : i32Ty();
            return builder->CreateLoad(innerTy, val, "deref");
        }
        if (expr.op == "~") return builder->CreateNot(val);
        return val;
    }

    case Expr::Kind::Borrow: {
        // &x → return address of x
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            auto* addr = getVarAddr(expr.inner->name);
            return addr ? addr : llvm::Constant::getNullValue(ptrTy());
        }
        return expr.inner ? genExpr(*expr.inner, func) : llvm::Constant::getNullValue(ptrTy());
    }

    case Expr::Kind::BorrowMut: {
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            auto* addr = getVarAddr(expr.inner->name);
            return addr ? addr : llvm::Constant::getNullValue(ptrTy());
        }
        return expr.inner ? genExpr(*expr.inner, func) : llvm::Constant::getNullValue(ptrTy());
    }

    case Expr::Kind::Move: {
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            auto* info = lookupVar(expr.inner->name);
            if (info && info->addr) {
                auto* val = builder->CreateLoad(info->type, info->addr, expr.inner->name + "_move");
                info->isMoved = true;
                return val;
            }
        }
        return expr.inner ? genExpr(*expr.inner, func) : llvm::Constant::getNullValue(i32Ty());
    }

    case Expr::Kind::New: {
        llvm::Type* innerTy = expr.typeArg ? toLLVM(*expr.typeArg) : i32Ty();
        uint64_t sz = sizeOf(innerTy);
        auto* heapPtr = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(i64Ty(), sz)}, "new_heap");
        if (expr.inner) {
            auto* initVal = genExpr(*expr.inner, func);
            builder->CreateStore(initVal, heapPtr);
        }
        return heapPtr;
    }

    case Expr::Kind::Assign: {
        auto* rval = expr.rhs ? genExpr(*expr.rhs, func) : nullptr;
        if (!rval) return llvm::Constant::getNullValue(i32Ty());
        if (expr.lhs) {
            if (expr.lhs->kind == Expr::Kind::Ident) {
                auto* info = lookupVar(expr.lhs->name);
                if (info && info->addr) builder->CreateStore(rval, info->addr);
            } else if (expr.lhs->kind == Expr::Kind::UnaryOp && expr.lhs->op == "*") {
                // *ptr = val
                auto* ptrVal = expr.lhs->inner ? genExpr(*expr.lhs->inner, func) : nullptr;
                if (ptrVal) builder->CreateStore(rval, ptrVal);
            }
        }
        return rval;
    }

    case Expr::Kind::Call: {
        // Resolve function name
        std::string funcName;
        if (expr.callee && expr.callee->kind == Expr::Kind::Ident)
            funcName = expr.callee->name;
        else if (expr.callee && expr.callee->kind == Expr::Kind::Member)
            funcName = expr.callee->field;

        // Generate argument values
        std::vector<llvm::Value*> argVals;
        for (auto& a : expr.args) {
            auto* v = genExpr(*a, func);
            if (v) argVals.push_back(v);
        }

        // Special case: printf
        if (funcName == "printf" || funcName == "fprintf") {
            auto* pfn = getOrDeclarePrintf();
            auto* ft  = pfn->getFunctionType();
            return builder->CreateCall(ft, pfn, argVals, "");
        }

        // Look up declared function
        auto* calleeFn = mod->getFunction(funcName);
        if (!calleeFn) {
            // Declare on-demand as extern with inferred types
            std::vector<llvm::Type*> paramTys;
            for (auto* v : argVals) paramTys.push_back(v->getType());
            auto* ftype = llvm::FunctionType::get(i32Ty(), paramTys, false);
            calleeFn = llvm::Function::Create(ftype, llvm::Function::ExternalLinkage,
                                               funcName, mod.get());
        }

        auto* callTy = calleeFn->getFunctionType();
        // Truncate or pad args if needed (for safety)
        std::vector<llvm::Value*> finalArgs;
        for (size_t i = 0; i < callTy->getNumParams() && i < argVals.size(); ++i)
            finalArgs.push_back(argVals[i]);
        if (callTy->isVarArg())
            for (size_t i = callTy->getNumParams(); i < argVals.size(); ++i)
                finalArgs.push_back(argVals[i]);

        auto* ret = builder->CreateCall(callTy, calleeFn, finalArgs);
        return ret->getType()->isVoidTy()
            ? (llvm::Value*)llvm::Constant::getNullValue(i32Ty())
            : ret;
    }

    case Expr::Kind::Member: {
        // Simple field access — struct layout is not yet implemented
        // For now, just evaluate the object and return zero
        if (expr.object) genExpr(*expr.object, func);
        addError(expr.line, "struct field access codegen not yet implemented");
        return llvm::Constant::getNullValue(i32Ty());
    }

    case Expr::Kind::Index: {
        // arr[idx] → load from (arr + idx)
        auto* arrVal = expr.object ? genExpr(*expr.object, func) : nullptr;
        auto* idxVal = expr.inner  ? genExpr(*expr.inner, func)  : nullptr;
        if (!arrVal || !idxVal) return llvm::Constant::getNullValue(i32Ty());

        auto ft = tc.getExprType(&expr);
        llvm::Type* elemTy = ft ? toLLVM(*ft) : i32Ty();
        auto* elemPtr = builder->CreateGEP(elemTy, arrVal, idxVal, "idx");
        return builder->CreateLoad(elemTy, elemPtr, "elem");
    }

    case Expr::Kind::Cast: {
        auto* val = expr.inner ? genExpr(*expr.inner, func) : nullptr;
        if (!val || !expr.typeArg) return val ? val : llvm::Constant::getNullValue(i32Ty());
        llvm::Type* destTy = toLLVM(*expr.typeArg);
        if (val->getType() == destTy) return val;
        if (destTy->isIntegerTy() && val->getType()->isIntegerTy())
            return builder->CreateIntCast(val, destTy, true, "cast");
        if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
            return builder->CreateSIToFP(val, destTy, "cast");
        if (destTy->isIntegerTy() && val->getType()->isDoubleTy())
            return builder->CreateFPToSI(val, destTy, "cast");
        return builder->CreateBitCast(val, destTy, "cast");
    }

    default:
        return llvm::Constant::getNullValue(i32Ty());
    }
}

// ─── Top-level entry ──────────────────────────────────────────────────────────

void Codegen::generate(const Program& prog) {
    for (auto& d : prog.decls) genDecl(*d);
}

} // namespace ferrum
