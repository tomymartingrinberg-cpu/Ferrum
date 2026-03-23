#include "ferrum/TypeChecker.h"
#include <sstream>

namespace ferrum {

// ─── FerType::toString ────────────────────────────────────────────────────────

std::string FerType::toString() const {
    switch (kind) {
    case Kind::Void:      return "void";
    case Kind::Int:       return "int";
    case Kind::Float:     return "float";
    case Kind::Char:      return "char";
    case Kind::Bool:      return "bool";
    case Kind::Generic:   return name;
    case Kind::Struct:    return name;
    case Kind::Pointer:   return (inner ? inner->toString() : "?") + "*" + (isUnsafe ? " unsafe" : "");
    case Kind::Borrow:    return (inner ? inner->toString() : "?") + "&";
    case Kind::BorrowMut: return (inner ? inner->toString() : "?") + "&mut";
    case Kind::Function:  {
        std::string s = "(";
        for (size_t i = 0; i < paramTypes.size(); ++i) {
            s += paramTypes[i]->toString();
            if (i+1 < paramTypes.size()) s += ", ";
        }
        s += ") -> " + (returnType ? returnType->toString() : "void");
        return s;
    }
    }
    return "?";
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

void TypeChecker::addError(int line, const std::string& msg) {
    errors.push_back({line, msg});
}

void TypeChecker::setExprType(const Expr& e, std::shared_ptr<FerType> t) {
    exprTypes[&e] = std::move(t);
}

std::shared_ptr<FerType> TypeChecker::getExprType(const Expr* e) const {
    auto it = exprTypes.find(e);
    return it != exprTypes.end() ? it->second : nullptr;
}

void TypeChecker::pushScope() { scopes.push_back({}); }

void TypeChecker::popScope() {
    if (!scopes.empty()) scopes.pop_back();
}

void TypeChecker::declare(const std::string& name, std::shared_ptr<FerType> type, int line) {
    if (scopes.empty()) return;
    if (scopes.back().count(name)) {
        addError(line, "redefinition of '" + name + "'");
    }
    scopes.back()[name] = std::move(type);
}

std::shared_ptr<FerType> TypeChecker::lookup(const std::string& name, int line) {
    for (int i = (int)scopes.size()-1; i >= 0; --i) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return it->second;
    }
    addError(line, "use of undeclared variable '" + name + "'");
    return FerType::makeInt(); // recover
}

// ─── Type resolution ──────────────────────────────────────────────────────────

std::shared_ptr<FerType> TypeChecker::resolveTypeRef(const TypeRef& ref) {
    switch (ref.kind) {
    case TypeRef::Kind::Named: {
        // Check generic substitution first
        if (typeParamSubst.count(ref.name)) return typeParamSubst.at(ref.name);
        if (ref.name == "int")   return FerType::makeInt();
        if (ref.name == "float") return FerType::makeFloat();
        if (ref.name == "char")  return FerType::makeChar();
        if (ref.name == "bool")  return FerType::makeBool();
        if (ref.name == "void")  return FerType::makeVoid();
        // Could be a generic type param or struct
        if (structs.count(ref.name)) return FerType::makeStruct(ref.name);
        // Assume it's a generic type param or unknown extern type
        return FerType::makeGeneric(ref.name);
    }
    case TypeRef::Kind::Pointer:
        if (!ref.inner) return FerType::makePtr(FerType::makeVoid());
        return FerType::makePtr(resolveTypeRef(*ref.inner), ref.isUnsafe);
    case TypeRef::Kind::Borrow:
        if (!ref.inner) return FerType::makeBorrow(FerType::makeVoid(), false);
        return FerType::makeBorrow(resolveTypeRef(*ref.inner), false);
    case TypeRef::Kind::BorrowMut:
        if (!ref.inner) return FerType::makeBorrow(FerType::makeVoid(), true);
        return FerType::makeBorrow(resolveTypeRef(*ref.inner), true);
    case TypeRef::Kind::Array:
        if (!ref.inner) return FerType::makePtr(FerType::makeVoid());
        return FerType::makePtr(resolveTypeRef(*ref.inner));
    }
    return FerType::makeVoid();
}

bool TypeChecker::typesCompatible(const FerType& a, const FerType& b) const {
    // Generic params are compatible with anything.
    if (a.isGenericParam() || b.isGenericParam()) return true;
    // Numeric promotion: int/float/char are interchangeable.
    if (a.isNumeric() && b.isNumeric()) return true;
    // Pointer types (including borrows).
    if (a.isPointerLike() && b.isPointerLike()) {
        // A raw pointer with no inner type info is compatible with anything.
        if (!a.inner || !b.inner) return true;
        // void* (null literal) is compatible with any pointer type.
        if (a.inner->kind == FerType::Kind::Void ||
            b.inner->kind == FerType::Kind::Void) return true;
        return typesCompatible(*a.inner, *b.inner);
    }
    return a.kind == b.kind;
}

// ─── Known C library functions ────────────────────────────────────────────────

void TypeChecker::registerCHeader(const std::string& header) {
    auto voidTy = FerType::makeVoid();
    auto intTy  = FerType::makeInt();
    auto chrTy  = FerType::makeChar();
    auto ptrChr = FerType::makePtr(chrTy);
    auto ptrVoid= FerType::makePtr(voidTy);

    if (header == "stdio.h" || header == "cstdio") {
        FuncInfo printf_info;
        printf_info.returnType = intTy;
        printf_info.paramTypes = {ptrChr};
        printf_info.isVariadic = true;
        printf_info.isExtern   = true;
        functions["printf"]  = printf_info;
        functions["fprintf"] = printf_info;
        functions["scanf"]   = printf_info;
        functions["puts"] = {{{ptrChr}}, intTy, {}, false, true};
    }
    if (header == "stdlib.h" || header == "cstdlib") {
        functions["malloc"] = {{{intTy}}, ptrVoid, {}, false, true};
        functions["free"]   = {{{ptrVoid}}, voidTy, {}, false, true};
        functions["exit"]   = {{{intTy}}, voidTy, {}, false, true};
        functions["atoi"]   = {{{ptrChr}}, intTy, {}, false, true};
    }
    if (header == "string.h" || header == "cstring") {
        functions["strlen"] = {{{ptrChr}}, intTy, {}, false, true};
        functions["strcmp"] = {{{ptrChr, ptrChr}}, intTy, {}, false, true};
        functions["strcpy"] = {{{ptrChr, ptrChr}}, ptrChr, {}, false, true};
        functions["memcpy"] = {{{ptrVoid, ptrVoid, intTy}}, ptrVoid, {}, false, true};
        functions["memset"] = {{{ptrVoid, intTy, intTy}}, ptrVoid, {}, false, true};
    }
    if (header == "math.h" || header == "cmath") {
        auto fltTy = FerType::makeFloat();
        functions["sqrt"]  = {{{fltTy}}, fltTy, {}, false, true};
        functions["pow"]   = {{{fltTy, fltTy}}, fltTy, {}, false, true};
        functions["fabs"]  = {{{fltTy}}, fltTy, {}, false, true};
        functions["floor"] = {{{fltTy}}, fltTy, {}, false, true};
        functions["ceil"]  = {{{fltTy}}, fltTy, {}, false, true};
    }
}

// ─── First pass: collect declarations ─────────────────────────────────────────

void TypeChecker::collectDecl(const Decl& decl) {
    switch (decl.kind) {
    case Decl::Kind::Function: {
        FuncInfo info;
        info.typeParams = decl.typeParams;
        // Temporarily add type params as generics for resolving param types
        for (auto& tp : decl.typeParams)
            typeParamSubst[tp] = FerType::makeGeneric(tp);
        for (auto& p : decl.params)
            info.paramTypes.push_back(resolveTypeRef(p.type));
        info.returnType = decl.returnType
            ? resolveTypeRef(*decl.returnType)
            : FerType::makeVoid();
        for (auto& tp : decl.typeParams)
            typeParamSubst.erase(tp);
        functions[decl.funcName] = std::move(info);
        break;
    }
    case Decl::Kind::Struct: {
        StructInfo info;
        info.typeParams = decl.structTypeParams;
        for (auto& f : decl.fields) {
            info.fieldNames.push_back(f.name);
            info.fieldTypes.push_back(resolveTypeRef(f.type));
        }
        structs[decl.structName] = std::move(info);
        break;
    }
    case Decl::Kind::Import:
        registerCHeader(decl.importPath);
        break;
    case Decl::Kind::ExternBlock:
        for (auto& d : decl.externDecls) collectDecl(*d);
        break;
    }
}

// ─── Expression type inference ────────────────────────────────────────────────

std::shared_ptr<FerType> TypeChecker::checkExpr(Expr& expr) {
    std::shared_ptr<FerType> result;

    switch (expr.kind) {
    case Expr::Kind::IntLit:    result = FerType::makeInt();   break;
    case Expr::Kind::FloatLit:  result = FerType::makeFloat(); break;
    case Expr::Kind::BoolLit:   result = FerType::makeBool();  break;
    case Expr::Kind::NullLit:   result = FerType::makePtr(FerType::makeVoid()); break;
    case Expr::Kind::StringLit: result = FerType::makePtr(FerType::makeChar()); break;

    case Expr::Kind::Ident: {
        result = lookup(expr.name, expr.line);
        break;
    }

    case Expr::Kind::BinOp: {
        auto lTy = expr.lhs ? checkExpr(*expr.lhs) : FerType::makeInt();
        auto rTy = expr.rhs ? checkExpr(*expr.rhs) : FerType::makeInt();
        const std::string& op = expr.op;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (!typesCompatible(*lTy, *rTy))
                addError(expr.line, "type mismatch in comparison: " + lTy->toString() + " vs " + rTy->toString());
            result = FerType::makeBool();
        } else if (op == "&&" || op == "||") {
            result = FerType::makeBool();
        } else {
            if (!typesCompatible(*lTy, *rTy))
                addError(expr.line, "type mismatch in binary op '" + op + "': " + lTy->toString() + " vs " + rTy->toString());
            result = lTy;
        }
        break;
    }

    case Expr::Kind::UnaryOp: {
        auto innerTy = expr.inner ? checkExpr(*expr.inner) : FerType::makeInt();
        if (expr.op == "*") {
            // Dereference
            if (innerTy->isPointerLike() && innerTy->inner)
                result = innerTy->inner;
            else {
                addError(expr.line, "cannot dereference non-pointer type '" + innerTy->toString() + "'");
                result = FerType::makeInt();
            }
        } else if (expr.op == "!") {
            result = FerType::makeBool();
        } else {
            result = innerTy;
        }
        break;
    }

    case Expr::Kind::Borrow: {
        auto innerTy = expr.inner ? checkExpr(*expr.inner) : FerType::makeVoid();
        result = FerType::makeBorrow(innerTy, false);
        break;
    }

    case Expr::Kind::BorrowMut: {
        auto innerTy = expr.inner ? checkExpr(*expr.inner) : FerType::makeVoid();
        result = FerType::makeBorrow(innerTy, true);
        break;
    }

    case Expr::Kind::Move: {
        result = expr.inner ? checkExpr(*expr.inner) : FerType::makeVoid();
        break;
    }

    case Expr::Kind::New: {
        auto innerTy = expr.typeArg ? resolveTypeRef(*expr.typeArg) : FerType::makeVoid();
        if (expr.inner) {
            auto valTy = checkExpr(*expr.inner);
            if (!typesCompatible(*innerTy, *valTy))
                addError(expr.line, "new<" + innerTy->toString() + "> initialized with " + valTy->toString());
        }
        result = FerType::makePtr(innerTy);
        break;
    }

    case Expr::Kind::Call: {
        // Resolve callee
        std::string funcName;
        if (expr.callee && expr.callee->kind == Expr::Kind::Ident)
            funcName = expr.callee->name;
        else if (expr.callee && expr.callee->kind == Expr::Kind::Member) {
            // method call: obj.method(args)
            if (expr.callee->object) checkExpr(*expr.callee->object);
            funcName = expr.callee->field;
        } else if (expr.callee) {
            checkExpr(*expr.callee);
        }

        // Check args
        for (auto& a : expr.args) checkExpr(*a);

        auto it = functions.find(funcName);
        if (it == functions.end()) {
            // Unknown function — allow it (might be a C function not yet declared)
            // Emit warning only for clearly non-extern calls
            result = FerType::makeVoid();
        } else {
            auto& info = it->second;
            if (!info.isVariadic && expr.args.size() != info.paramTypes.size()) {
                addError(expr.line, "function '" + funcName + "' called with " +
                    std::to_string(expr.args.size()) + " argument(s), expected " +
                    std::to_string(info.paramTypes.size()));
            } else if (!info.isVariadic) {
                for (size_t i = 0; i < expr.args.size() && i < info.paramTypes.size(); ++i) {
                    auto argTy = getExprType(expr.args[i].get());
                    if (argTy && !typesCompatible(*argTy, *info.paramTypes[i])) {
                        addError(expr.line, "argument " + std::to_string(i+1) +
                            " to '" + funcName + "': expected " + info.paramTypes[i]->toString() +
                            ", got " + argTy->toString());
                    }
                }
            }
            result = info.returnType ? info.returnType : FerType::makeVoid();
        }
        break;
    }

    case Expr::Kind::Assign: {
        auto rTy = expr.rhs ? checkExpr(*expr.rhs) : FerType::makeVoid();
        std::shared_ptr<FerType> lTy;
        if (expr.lhs) {
            lTy = checkExpr(*expr.lhs);
            if (!typesCompatible(*lTy, *rTy))
                addError(expr.line, "cannot assign " + rTy->toString() + " to " + lTy->toString());
        }
        result = lTy ? lTy : rTy;
        break;
    }

    case Expr::Kind::Member: {
        auto objTy = expr.object ? checkExpr(*expr.object) : FerType::makeVoid();
        // Resolve through pointer/borrow
        auto* baseTy = objTy.get();
        while (baseTy && baseTy->isPointerLike() && baseTy->inner)
            baseTy = baseTy->inner.get();
        if (baseTy && baseTy->kind == FerType::Kind::Struct) {
            auto sit = structs.find(baseTy->name);
            if (sit != structs.end()) {
                for (size_t i = 0; i < sit->second.fieldNames.size(); ++i) {
                    if (sit->second.fieldNames[i] == expr.field) {
                        result = sit->second.fieldTypes[i];
                        break;
                    }
                }
                if (!result)
                    addError(expr.line, "struct '" + baseTy->name + "' has no field '" + expr.field + "'");
            }
        }
        if (!result) result = FerType::makeVoid();
        break;
    }

    case Expr::Kind::Index: {
        auto objTy = expr.object ? checkExpr(*expr.object) : FerType::makeVoid();
        if (expr.inner) checkExpr(*expr.inner);
        if (objTy->isPointerLike() && objTy->inner)
            result = objTy->inner;
        else
            result = FerType::makeVoid();
        break;
    }

    case Expr::Kind::Cast: {
        if (expr.inner) checkExpr(*expr.inner);
        result = expr.typeArg ? resolveTypeRef(*expr.typeArg) : FerType::makeVoid();
        break;
    }

    default:
        result = FerType::makeVoid();
    }

    if (!result) result = FerType::makeVoid();
    setExprType(expr, result);
    return result;
}

// ─── Statement type checking ──────────────────────────────────────────────────

void TypeChecker::checkStmt(Stmt& stmt) {
    switch (stmt.kind) {
    case Stmt::Kind::Block:
        pushScope();
        for (auto& s : stmt.stmts) checkStmt(*s);
        popScope();
        break;

    case Stmt::Kind::VarDecl: {
        std::shared_ptr<FerType> declaredTy;
        if (stmt.varType) declaredTy = resolveTypeRef(*stmt.varType);

        std::shared_ptr<FerType> initTy;
        if (stmt.varInit) initTy = checkExpr(*stmt.varInit);

        std::shared_ptr<FerType> varTy = declaredTy ? declaredTy : initTy;
        if (!varTy) varTy = FerType::makeVoid();

        if (declaredTy && initTy && !typesCompatible(*declaredTy, *initTy)) {
            addError(stmt.line, "cannot initialize '" + stmt.varName + "' of type " +
                declaredTy->toString() + " with " + initTy->toString());
        }
        declare(stmt.varName, varTy, stmt.line);
        break;
    }

    case Stmt::Kind::Expr:
        if (stmt.expr) checkExpr(*stmt.expr);
        break;

    case Stmt::Kind::Return: {
        if (stmt.expr) {
            auto retTy = checkExpr(*stmt.expr);
            if (currentReturnType && !typesCompatible(*currentReturnType, *retTy))
                addError(stmt.line, "return type mismatch: expected " +
                    currentReturnType->toString() + ", got " + retTy->toString());
        }
        break;
    }

    case Stmt::Kind::If:
        if (stmt.condition) checkExpr(*stmt.condition);
        if (stmt.thenBranch) checkStmt(*stmt.thenBranch);
        if (stmt.elseBranch) checkStmt(*stmt.elseBranch);
        break;

    case Stmt::Kind::While:
        if (stmt.condition) checkExpr(*stmt.condition);
        if (stmt.body) checkStmt(*stmt.body);
        break;

    case Stmt::Kind::For:
        pushScope();
        if (stmt.init) checkStmt(*stmt.init);
        if (stmt.condition) checkExpr(*stmt.condition);
        if (stmt.increment) checkExpr(*stmt.increment);
        if (stmt.body) checkStmt(*stmt.body);
        popScope();
        break;

    case Stmt::Kind::Unsafe:
        for (auto& s : stmt.stmts) checkStmt(*s);
        break;

    default: break;
    }
}

// ─── Declaration type checking ────────────────────────────────────────────────

void TypeChecker::checkDecl(Decl& decl) {
    if (decl.kind == Decl::Kind::Function) {
        auto it = functions.find(decl.funcName);
        if (it == functions.end()) return;
        auto& info = it->second;

        // Add type params to substitution as generic placeholders
        for (auto& tp : decl.typeParams)
            typeParamSubst[tp] = FerType::makeGeneric(tp);

        currentReturnType = info.returnType;
        pushScope();
        for (size_t i = 0; i < decl.params.size() && i < info.paramTypes.size(); ++i)
            declare(decl.params[i].name, info.paramTypes[i], decl.line);
        if (decl.funcBody) checkStmt(*decl.funcBody);
        popScope();

        for (auto& tp : decl.typeParams)
            typeParamSubst.erase(tp);
    } else if (decl.kind == Decl::Kind::ExternBlock) {
        for (auto& d : decl.externDecls) checkDecl(*d);
    }
}

// ─── Top-level entry point ────────────────────────────────────────────────────

void TypeChecker::check(Program& prog) {
    // Pass 1: collect all signatures
    for (auto& d : prog.decls) collectDecl(*d);
    // Pass 2: type-check bodies
    for (auto& d : prog.decls) checkDecl(*d);
}

} // namespace ferrum
