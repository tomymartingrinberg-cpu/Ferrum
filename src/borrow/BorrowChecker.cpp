#include "ferrum/BorrowChecker.h"
#include <sstream>

namespace ferrum {

void BorrowChecker::addError(BorrowError::Kind kind, const std::string& var,
                              int line, const std::string& msg) {
    errors.push_back({kind, var, line, msg});
}

void BorrowChecker::pushScope() {
    scopes.push_back({});
    currentScopeLevel++;
}

void BorrowChecker::popScope() {
    if (scopes.empty()) return;
    currentScopeLevel--;

    // Before dropping: check for borrows that outlive their owners.
    // If a var in this scope is borrowed by a var in an outer scope, that's a dangling borrow.
    for (auto& [name, state] : scopes.back()) {
        // If some outer-scope variable borrows this variable, emit BorrowOutlivesOwner
        for (int i = 0; i < (int)scopes.size()-1; ++i) {
            for (auto& [outerName, outerState] : scopes[i]) {
                if (outerState.borrowsFrom == name &&
                    outerState.borrowedScopeLevel == currentScopeLevel+1) {
                    addError(BorrowError::Kind::BorrowOutlivesOwner, outerName,
                        state.declLine,
                        "'" + outerName + "' borrows '" + name +
                        "' which is dropped here");
                }
            }
        }
        // Release any borrow this variable holds on another variable
        if (!state.borrowsFrom.empty()) {
            releaseBorrow(state.borrowsFrom, state.isMutBorrow);
        }
    }

    scopes.pop_back();
}

void BorrowChecker::declareVar(const std::string& name, VarState state) {
    if (!scopes.empty()) {
        state.scopeLevel = currentScopeLevel;
        scopes.back()[name] = state;
    }
}

VarState* BorrowChecker::lookupVar(const std::string& name) {
    for (int i = (int)scopes.size() - 1; i >= 0; i--) {
        auto it = scopes[i].find(name);
        if (it != scopes[i].end()) return &it->second;
    }
    return nullptr;
}

void BorrowChecker::recordMove(const std::string& name, int line) {
    VarState* s = lookupVar(name);
    if (!s) return;
    if (s->ownership == VarState::Ownership::Moved) {
        addError(BorrowError::Kind::UseAfterMove, name, line,
            "use of moved value '" + name + "'");
        return;
    }
    if (s->borrowCount > 0 || s->hasMutBorrow) {
        addError(BorrowError::Kind::MutateWhileBorrowed, name, line,
            "cannot move '" + name + "' because it is borrowed");
        return;
    }
    s->ownership = VarState::Ownership::Moved;
}

void BorrowChecker::recordBorrow(const std::string& name, bool isMut, int line,
                                  const std::string& declaredVar) {
    VarState* s = lookupVar(name);
    if (!s) return;

    if (s->ownership == VarState::Ownership::Moved) {
        addError(BorrowError::Kind::UseAfterMove, name, line,
            "cannot borrow moved value '" + name + "'");
        return;
    }
    if (isMut) {
        if (s->borrowCount > 0) {
            addError(BorrowError::Kind::MutableBorrowWhileBorrowed, name, line,
                "cannot borrow '" + name + "' as mutable because it is also borrowed as immutable");
            return;
        }
        if (s->hasMutBorrow) {
            addError(BorrowError::Kind::MutableBorrowWhileBorrowed, name, line,
                "cannot borrow '" + name + "' as mutable more than once at a time");
            return;
        }
        s->hasMutBorrow = true;
        s->ownership = VarState::Ownership::BorrowedMut;
    } else {
        if (s->hasMutBorrow) {
            addError(BorrowError::Kind::MutableBorrowWhileBorrowed, name, line,
                "cannot borrow '" + name + "' as immutable because it is also borrowed as mutable");
            return;
        }
        s->borrowCount++;
        s->ownership = VarState::Ownership::Borrowed;
    }

    // Register lifetime tracking on the borrow variable (if we know its name)
    if (!declaredVar.empty()) {
        VarState* bv = lookupVar(declaredVar);
        if (bv) {
            bv->borrowsFrom = name;
            bv->borrowedScopeLevel = s->scopeLevel;
        }
    }
}

void BorrowChecker::releaseBorrow(const std::string& name, bool isMut) {
    VarState* s = lookupVar(name);
    if (!s) return;
    if (isMut) {
        s->hasMutBorrow = false;
    } else {
        if (s->borrowCount > 0) s->borrowCount--;
    }
    if (s->borrowCount == 0 && !s->hasMutBorrow) {
        s->ownership = VarState::Ownership::Owned;
    }
}

// ─── Top-level check ──────────────────────────────────────────────────────────

void BorrowChecker::check(const Program& prog) {
    for (auto& d : prog.decls) checkDecl(*d);
}

void BorrowChecker::checkDecl(const Decl& decl) {
    if (decl.kind == Decl::Kind::Function) {
        pushScope();
        for (auto& p : decl.params) {
            VarState s;
            s.declLine = decl.line;
            // Check if param type is an unsafe pointer outside unsafe context
            if (p.type.isUnsafe && !inUnsafe()) {
                // Allow in function signatures (the body must use it safely)
                s.isUnsafePtr = true;
            }
            declareVar(p.name, s);
        }
        if (decl.funcBody) checkStmt(*decl.funcBody);
        popScope();
    } else if (decl.kind == Decl::Kind::ExternBlock) {
        for (auto& d : decl.externDecls) checkDecl(*d);
    }
}

void BorrowChecker::checkStmt(const Stmt& stmt) {
    switch (stmt.kind) {
    case Stmt::Kind::Block: {
        pushScope();
        for (auto& s : stmt.stmts) checkStmt(*s);
        popScope();
        break;
    }
    case Stmt::Kind::VarDecl: {
        VarState s;
        s.declLine = stmt.line;

        bool isUnsafePtr = stmt.varType && stmt.varType->isUnsafe;
        if (isUnsafePtr) {
            s.isUnsafePtr = true;
            if (!inUnsafe()) {
                addError(BorrowError::Kind::UnsafeOutsideUnsafeBlock,
                    stmt.varName, stmt.line,
                    "unsafe pointer '" + stmt.varName +
                    "' can only be declared inside an unsafe block");
            }
        }

        if (stmt.varInit) {
            if (stmt.varInit->kind == Expr::Kind::New) s.isHeap = true;

            // Track borrow: if init is &x or &mut x, record borrowsFrom
            std::string borrowTarget;
            bool borrowIsMut = false;
            if (stmt.varInit->kind == Expr::Kind::Borrow ||
                stmt.varInit->kind == Expr::Kind::BorrowMut) {
                borrowIsMut = (stmt.varInit->kind == Expr::Kind::BorrowMut);
                if (stmt.varInit->inner &&
                    stmt.varInit->inner->kind == Expr::Kind::Ident) {
                    borrowTarget = stmt.varInit->inner->name;
                }
            }

            checkExpr(*stmt.varInit);

            // After checkExpr recorded the borrow, annotate the borrow var
            if (!borrowTarget.empty()) {
                VarState* bs = lookupVar(borrowTarget);
                if (bs) {
                    // Will be registered in recordBorrow; also set on the new var after declare
                    s.borrowsFrom        = borrowTarget;
                    s.borrowedScopeLevel = bs->scopeLevel;
                    s.isMutBorrow        = borrowIsMut;
                }
            }
        }
        declareVar(stmt.varName, s);
        break;
    }
    case Stmt::Kind::Expr:
        if (stmt.expr) checkExpr(*stmt.expr);
        break;
    case Stmt::Kind::Return:
        if (stmt.expr) checkExpr(*stmt.expr);
        break;
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
        unsafeDepth++;
        for (auto& s : stmt.stmts) checkStmt(*s);
        unsafeDepth--;
        break;
    default: break;
    }
}

void BorrowChecker::checkExpr(const Expr& expr) {
    switch (expr.kind) {
    case Expr::Kind::Move:
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            VarState* s = lookupVar(expr.inner->name);
            if (s && s->ownership == VarState::Ownership::Moved) {
                addError(BorrowError::Kind::UseAfterMove, expr.inner->name, expr.line,
                    "use of moved value '" + expr.inner->name + "'");
            } else {
                recordMove(expr.inner->name, expr.line);
            }
        } else if (expr.inner) {
            checkExpr(*expr.inner);
        }
        break;

    case Expr::Kind::Borrow:
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            recordBorrow(expr.inner->name, false, expr.line);
        } else if (expr.inner) {
            checkExpr(*expr.inner);
        }
        break;

    case Expr::Kind::BorrowMut:
        if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
            recordBorrow(expr.inner->name, true, expr.line);
        } else if (expr.inner) {
            checkExpr(*expr.inner);
        }
        break;

    case Expr::Kind::Ident: {
        VarState* s = lookupVar(expr.name);
        if (s && s->ownership == VarState::Ownership::Moved) {
            addError(BorrowError::Kind::UseAfterMove, expr.name, expr.line,
                "use of moved value '" + expr.name + "'");
        }
        break;
    }

    case Expr::Kind::UnaryOp:
        // Dereference of unsafe pointer outside unsafe block is an error
        if (expr.op == "*" && !inUnsafe()) {
            if (expr.inner && expr.inner->kind == Expr::Kind::Ident) {
                VarState* s = lookupVar(expr.inner->name);
                if (s && s->isUnsafePtr) {
                    addError(BorrowError::Kind::UnsafeOutsideUnsafeBlock,
                        expr.inner->name, expr.line,
                        "dereference of unsafe pointer '" + expr.inner->name +
                        "' outside unsafe block");
                }
            }
        }
        if (expr.inner) checkExpr(*expr.inner);
        break;

    case Expr::Kind::Assign:
        // Check the rhs value normally.
        if (expr.rhs) checkExpr(*expr.rhs);
        // For the lhs: only validate borrow state.
        // Do NOT call checkExpr on the lhs ident — assigning TO a moved variable
        // is valid re-initialization and must not be reported as use-after-move.
        if (expr.lhs && expr.lhs->kind == Expr::Kind::Ident) {
            VarState* s = lookupVar(expr.lhs->name);
            if (s) {
                if (s->borrowCount > 0 || s->hasMutBorrow) {
                    addError(BorrowError::Kind::MutateWhileBorrowed,
                        expr.lhs->name, expr.line,
                        "cannot assign to '" + expr.lhs->name + "' because it is borrowed");
                }
                // Re-assignment re-initializes a previously moved variable.
                if (s->ownership == VarState::Ownership::Moved)
                    s->ownership = VarState::Ownership::Owned;
            }
        } else if (expr.lhs) {
            // Non-ident lhs (e.g. *ptr = val): check it normally.
            checkExpr(*expr.lhs);
        }
        break;

    case Expr::Kind::Call:
        if (expr.callee) checkExpr(*expr.callee);
        for (auto& a : expr.args) checkExpr(*a);
        break;

    case Expr::Kind::BinOp:
        if (expr.lhs) checkExpr(*expr.lhs);
        if (expr.rhs) checkExpr(*expr.rhs);
        break;

    case Expr::Kind::Member:
    case Expr::Kind::Index:
        if (expr.object) checkExpr(*expr.object);
        if (expr.inner)  checkExpr(*expr.inner);
        break;

    case Expr::Kind::New:
        if (expr.inner) checkExpr(*expr.inner);
        break;

    default: break;
    }
}

} // namespace ferrum
