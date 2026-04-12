#pragma once
#include "AST.h"
#include "SymbolTable.h"
#include <unordered_map>
#include <unordered_set>
#include <iostream>

// =============================================================================
// CGIL SEMANTIC ANALYZER
// =============================================================================
//
// WHAT THIS PHASE DOES:
//   The Parser proves the program is GRAMMATICALLY correct — tokens in the right
//   order. The Semantic Analyzer proves the program is LOGICALLY correct — that
//   what the grammar says actually makes sense.
//
//   It catches errors that the grammar cannot catch:
//     - "fetch_sector requires Disk:Idle but my_disk is Disk:Fault"
//     - "Variable 'ctrl' was transferred with 'own' and cannot be used here"
//     - "Divine block is missing a success branch"
//     - "'shatter' used outside of a loop"
//     - "Call to unknown spell 'read_bufffer' (did you mean 'read_buffer'?)"
//
// TWO-PASS ARCHITECTURE:
//   The analyzer runs in two passes over the same AST:
//
//   PASS 1 — The Global Ledger (isPassOne = true)
//     Scans all top-level declarations and registers them. After Pass 1:
//     - Every rank, sigil, and legion is in the typeRegistry
//     - Every spell is in the spellRegistry
//     - Every hardware variable is in the symbol table
//
//     WHY: Without this pass, you couldn't call fetch_sector before its
//     definition in the file. The two-pass design eliminates header-ordering
//     hell — types and spells can be used before they appear in the file.
//
//   PASS 2 — The Crucible (isPassOne = false)
//     Walks every spell body, checking types, enforcing stances, and tracking
//     ownership. Every expression produces a TypeInfo. Every assignment that
//     is a stance transition updates the variable's currentStance.
//
// THE VISITOR PATTERN IN ACTION:
//   SemanticAnalyzer implements ASTVisitor, so it has a visit() method for
//   every AST node type. Walking the tree is just calling accept() on each
//   node, which calls the right visit() overload via virtual dispatch.
//
//   Expressions are visited via evaluate(), which calls accept() and returns
//   the resulting type stored in currentExprType. This "return value via
//   side effect" pattern (setting a member variable) is the standard approach
//   for visitor-based tree walkers.
//
// ERROR HANDLING:
//   Errors throw std::runtime_error with source location. The caller (main.cpp)
//   catches them. For V1, the first semantic error stops the compiler.
//   A production implementation would accumulate errors and continue.
//
// =============================================================================

class SemanticAnalyzer : public ASTVisitor {
public:
    SemanticAnalyzer() {
        // The global scope always exists. Hardware declarations live here.
        symbols.enterScope();
        registerPrimitives();
    }

    // Main entry point. Call this once after parsing.
    // Runs Pass 1 then Pass 2 over the entire program.
    void analyze(ProgramNode* program) {
        // PASS 1: Register all global declarations so Pass 2 can reference them.
        isPassOne = true;
        for (auto& decl : program->declarations) {
            decl->accept(*this);
        }

        // PASS 2: Check bodies, types, stances, and ownership.
        isPassOne = false;
        for (auto& decl : program->declarations) {
            decl->accept(*this);
        }
    }

    // =========================================================================
    // ASTVISITOR OVERRIDES
    // One method per node type. All 24 must be implemented or the class
    // cannot be instantiated (ASTVisitor has pure virtual methods for all of them).
    // =========================================================================

    // --- Declarations ---
    void visit(GrimoireDecl* node) override;
    void visit(RankDecl*     node) override;
    void visit(SigilDecl*    node) override;
    void visit(LegionDecl*   node) override;
    void visit(SpellDecl*    node) override;
    void visit(HardwareDecl* node) override;

    // --- Statements ---
    void visit(BlockStmt*    node) override;
    void visit(ExprStmt*     node) override;
    void visit(AssignStmt*   node) override;
    void visit(YieldStmt*    node) override;
    void visit(ShatterStmt*  node) override;
    void visit(SurgeStmt*    node) override;
    void visit(IfStmt*       node) override;
    void visit(ForeStmt*     node) override;
    void visit(WhirlStmt*    node) override;
    void visit(DestinedStmt* node) override;
    void visit(DivineStmt*   node) override;
    void visit(VarDeclStmt*  node) override;

    // --- Expressions ---
    void visit(BinaryExpr*     node) override;
    void visit(UnaryExpr*      node) override;
    void visit(PostfixExpr*    node) override;
    void visit(LiteralExpr*    node) override;
    void visit(IdentifierExpr* node) override;
    void visit(CallExpr*       node) override;
    void visit(AddressOfExpr*  node) override;

private:
    // =========================================================================
    // STATE TRACKED DURING ANALYSIS
    // =========================================================================

    // Which pass are we on?
    bool isPassOne = true;

    // The scoped variable state. Mutated constantly during Pass 2.
    SymbolTable symbols;

    // All user-defined and built-in types by name.
    // Populated in Pass 1. Read-only in Pass 2.
    // Key: source-level type name ("mark16", "Disk", "DiskError")
    // Value: the resolved TypeInfo for that type
    std::unordered_map<std::string, std::shared_ptr<TypeInfo>> typeRegistry;

    // All declared spells by name.
    // Populated in Pass 1. Read in Pass 2 to resolve calls.
    // Key: spell name ("fetch_sector", "kernel_panic")
    // Value: raw pointer to the SpellDecl AST node (owned by the AST, always valid)
    std::unordered_map<std::string, SpellDecl*> spellRegistry;

    // The type of the most recently evaluated expression.
    // Set by every expression visitor. Read by the caller immediately after.
    // This "return value via side effect" approach is standard for ASTVisitor.
    // NEVER read this field without having just called evaluate() or accept().
    std::shared_ptr<TypeInfo> currentExprType;

    // The spell currently being analyzed (set in visit(SpellDecl*) Pass 2).
    // Used by:
    //   - YieldStmt: to check the return type matches the spell's declaration
    //   - DivineStmt/DestinedStmt: to validate warden spell constraints
    //   - ShatterStmt/SurgeStmt: to detect misuse
    // nullptr when outside any spell body.
    SpellDecl* currentSpell = nullptr;

    // Loop nesting depth. Incremented on entering fore/whirl, decremented on exit.
    // shatter and surge are compile errors when loopDepth == 0.
    int loopDepth = 0;

    // =========================================================================
    // PRIVATE HELPERS
    // =========================================================================

    // Evaluate an expression node and return its resolved TypeInfo.
    // This is the standard way to "ask" what type an expression produces.
    // Internally calls node->accept(*this) which sets currentExprType via the
    // appropriate visit() overload, then returns currentExprType.
    //
    // Always call this instead of manually calling node->accept() and reading
    // currentExprType — this pattern makes the intent explicit.
    std::shared_ptr<TypeInfo> evaluate(Expr* node);

    // Look up a type name in the registry and return its TypeInfo.
    // Throws a semantic error with source location if the type is not found.
    // Use this instead of typeRegistry[] which silently inserts null entries.
    std::shared_ptr<TypeInfo> resolveType(Token nameToken);

    // Register all built-in Cgil primitive types in the typeRegistry.
    // Called once in the constructor before any source file is analyzed.
    //
    // FIXED: Now registers ALL Cgil primitives from the spec:
    //   mark16, mark32, soul16, soul32, addr, flow, rune, oath, scroll, abyss,
    //   deck, tuple
    //
    // Previously missing: mark32, soul32, flow, deck, tuple
    // Previously wrong:   abyss was registered as PRIMITIVE — now uses its
    //                     own PRIMITIVE entry (ABYSS TypeKind is removed;
    //                     abyss is a primitive with special void semantics).
    void registerPrimitives() {
        // Helper: add a primitive type with kind=PRIMITIVE
        auto addPrim = [&](const std::string& name) {
            typeRegistry[name] = std::make_shared<TypeInfo>(
                TypeInfo{TypeKind::PRIMITIVE, name}
            );
        };

        // Signed integers
        addPrim("mark16");
        addPrim("mark32");

        // Unsigned integers
        addPrim("soul16");
        addPrim("soul32");

        // Hardware address alias (soul16 semantically)
        addPrim("addr");

        // Floating point (emits kernel-mode warning)
        addPrim("flow");

        // 8-bit byte/char
        addPrim("rune");

        // Boolean (kept=1, forsaken=0)
        addPrim("oath");

        // Fat pointer string (rune* + soul16 length)
        addPrim("scroll");

        // Void return type
        addPrim("abyss");

        // Fixed-size array — note: deck[N] T is a composite type, but the
        // bare "deck" token is registered so consumeType() doesn't reject it.
        addPrim("deck");

        // Tuple boundary type — similarly registered for token recognition.
        addPrim("tuple");
    }

    // Format and throw a semantic error with source location.
    // [[noreturn]]: the compiler knows this never returns, suppressing
    // "control reaches end of non-void function" warnings in callers.
    [[noreturn]] void error(Token token, const std::string& message) {
        throw std::runtime_error(
            "[Semantic Error Line " + std::to_string(token.line) +
            ":" + std::to_string(token.column) + "] " + message
        );
    }
};
