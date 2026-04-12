#include "SemanticAnalyzer.h"

// =============================================================================
// PRIVATE HELPERS
// =============================================================================

// Evaluate an expression and return its type.
// This is the standard way to ask "what type does this expression produce?"
// Sets currentExprType as a side effect, then returns it.
std::shared_ptr<TypeInfo> SemanticAnalyzer::evaluate(Expr* node) {
    node->accept(*this);
    return currentExprType;
}

// Look up a type name in the registry. Error if not found.
// Use this instead of typeRegistry[] which silently inserts null on missing keys.
std::shared_ptr<TypeInfo> SemanticAnalyzer::resolveType(Token nameToken) {
    auto it = typeRegistry.find(nameToken.lexeme);
    if (it == typeRegistry.end()) {
        error(nameToken, "Unknown type '" + nameToken.lexeme + "'.");
    }
    return it->second;
}

// =============================================================================
// PASS 1 — THE GLOBAL LEDGER
// =============================================================================
// In Pass 1, we walk all top-level declarations and register every type, spell,
// and hardware variable that exists in the program. We do NOT look inside spell
// bodies. We do NOT check types of expressions.
//
// After Pass 1 completes, the typeRegistry and spellRegistry are fully populated.
// Pass 2 can then look up any type or spell by name without forward-declaration
// ordering concerns — this is how Cgil eliminates header-ordering hell.
//
// PATTERN: Every visitor checks `if (!isPassOne) return;` at the top.
//          This means Pass 2 re-enters the same visitor but skips immediately.
//          Pass 2 entry points are at the BOTTOM of each declaration visitor
//          in a clearly marked section.
// =============================================================================

// grimoire <hardware_defs.h>;   pact <stdlib.h>;
// Pass 1: Nothing to register. Includes are resolved at the C level.
// Pass 2: In a full implementation, pact in kernel context would warn here.
void SemanticAnalyzer::visit(GrimoireDecl* node) {
    // V1: No action required in either pass.
    // V1.5 TODO: Warn if node->isPact is true and we are in kernel context
    // (i.e., not inside a spell marked with pact-safe modifier).
    (void)node;
}

// rank DiskError { Timeout, HardwareFault, InvalidSector }
//
// PASS 1: Register "DiskError" in the typeRegistry with kind=RANK.
//         The variants (Timeout, HardwareFault, etc.) are NOT put in the symbol
//         table. They are accessed via scope resolution (DiskError::Timeout) and
//         are looked up directly from the RankDecl AST node by Pass 2 code that
//         handles rank variant references.
//
// PASS 2: No additional action needed — variants are validated at use sites.
void SemanticAnalyzer::visit(RankDecl* node) {
    if (!isPassOne) return;

    if (typeRegistry.count(node->name.lexeme)) {
        error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
    }

    typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
        TypeInfo{TypeKind::RANK, node->name.lexeme}
    );
}

// sigil Disk { stance Idle; stance Reading; soul16 sector_count; }
//
// PASS 1: Register the sigil type. Register any bound spells under a mangled name.
//         Name mangling: bound spell "emit" on sigil "ASTNode" -> "ASTNode_emit".
//         This prevents name collisions between top-level spells and bound spells.
//
// PASS 2: Validate that all field types exist in the registry.
//         Analyze the bodies of all bound spells.
void SemanticAnalyzer::visit(SigilDecl* node) {
    if (isPassOne) {
        if (typeRegistry.count(node->name.lexeme)) {
            error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
        }

        typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
            TypeInfo{TypeKind::SIGIL, node->name.lexeme}
        );

        // Register bound spells (V1.5 encapsulation).
        for (auto& boundSpell : node->boundSpells) {
            std::string mangledName = node->name.lexeme + "_" + boundSpell->name.lexeme;
            if (spellRegistry.count(mangledName)) {
                error(boundSpell->name, "Bound spell '" + mangledName + "' is already declared.");
            }
            spellRegistry[mangledName] = boundSpell.get();
        }
        return;
    }

    // Pass 2: Validate field types.
    for (auto& field : node->fields) {
        resolveType(field.type); // Errors if "soul16", "addr", etc. not found
    }

    // Pass 2: Analyze bound spell bodies.
    for (auto& boundSpell : node->boundSpells) {
        boundSpell->accept(*this);
    }
}

// legion SectorCache { mark16 sector_id; flow read_time; oath is_corrupted; }
//
// PASS 1: Register as SIGIL (V1 stub — no SoA transformation yet).
//         The distinct LEGION keyword is preserved for V2 to identify these nodes.
//
// PASS 2: Validate field types exist.
void SemanticAnalyzer::visit(LegionDecl* node) {
    if (isPassOne) {
        if (typeRegistry.count(node->name.lexeme)) {
            error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
        }
        // V1: Treated as SIGIL. V2 will use a LEGION TypeKind for SoA transforms.
        typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
            TypeInfo{TypeKind::SIGIL, node->name.lexeme}
        );
        return;
    }

    // Pass 2: Validate field types.
    for (auto& field : node->fields) {
        resolveType(field.type);
    }
}

// spell fetch_sector(...)   warden spell disk_irq()   conjure spell read_buffer(...)
//
// PASS 1: Register the spell in the spellRegistry. Validation of parameter types
//         and return types happens in Pass 2 when we have the full type registry.
//
// PASS 2: This is where the real work happens.
//   1. Set currentSpell so all nested visitors know the enclosing context.
//   2. Validate parameter types exist.
//   3. Open a scope and declare all parameters as symbols.
//   4. Walk every statement in the body.
//   5. Close the scope and clear currentSpell.
//
// FIXED: The original returned immediately on `!isPassOne`, meaning spell bodies
//        were NEVER analyzed. Pass 2 was a complete no-op for all spell content.
void SemanticAnalyzer::visit(SpellDecl* node) {
    if (isPassOne) {
        if (spellRegistry.count(node->name.lexeme)) {
            error(node->name, "Spell '" + node->name.lexeme + "' is already declared.");
        }
        spellRegistry[node->name.lexeme] = node;
        return;
    }

    // conjure spells are extern declarations — they have no body to analyze.
    if (node->isConjure) return;

    // --- WARDEN SPELL CONSTRAINT CHECK ---
    // warden spells are Interrupt Service Routines. They have strict rules
    // per the spec: no own, no ruin propagation.
    // We enforce this during body analysis by checking currentSpell->isWarden.

    // Set context so nested visitors know what spell they are inside.
    SpellDecl* previousSpell = currentSpell;
    currentSpell = node;

    // Enter the spell's scope. Parameters live here.
    symbols.enterScope();

    // Declare all parameters as symbols with their resolved types and stances.
    for (auto& param : node->params) {
        auto paramType = resolveType(param.type); // Errors if type unknown
        symbols.declare(
            param.name.lexeme,
            paramType,
            param.isOwned,
            param.stanceName.lexeme, // "" if no stance constraint
            false                    // not hardware
        );
    }

    // Walk the spell body. Every statement will be analyzed in this scope.
    for (auto& stmt : node->body) {
        stmt->accept(*this);
    }

    // Clean up.
    symbols.exitScope();
    currentSpell = previousSpell;
}

// leyline disk_status_port: rune @ 0x1F7;
// portline disk_data_port: soul16 @ 0x1F0;
//
// PASS 1: Validate the type exists, then declare as a global hardware variable.
//         isHardware=true distinguishes these from regular variables for the
//         `&name` address-of semantic — &hardware_var emits a compile-time
//         address constant, not C's address-of operator.
//
// PASS 2: No action — hardware is globally registered in Pass 1.
//
// FIXED: Now uses resolveType() instead of typeRegistry[] for safe lookup.
//        Now passes isHardware=true and isPortline to the symbol.
void SemanticAnalyzer::visit(HardwareDecl* node) {
    if (!isPassOne) return;

    auto hwType = resolveType(node->type); // Error if "rune" etc. not registered

    // Build a HARDWARE-kind TypeInfo wrapping the underlying data type.
    auto hardwareTypeInfo = std::make_shared<TypeInfo>(TypeInfo{
        TypeKind::HARDWARE,
        node->name.lexeme,
        nullptr, // no successType
        nullptr  // no ruinType
    });
    hardwareTypeInfo->isPortline = node->isPortline;

    // Declare in global scope (scopes[0]).
    // isHardware=true is critical for AddressOfExpr to emit the right C.
    symbols.declare(
        node->name.lexeme,
        hardwareTypeInfo,
        false, // not owned
        "",    // no stance
        true   // IS hardware
    );
}

// =============================================================================
// PASS 2 — THE CRUCIBLE
// Statement and expression visitors. These only run during Pass 2.
// Every visitor that is statement/expression-only starts with `if (isPassOne) return;`
// =============================================================================

// --- STATEMENT VISITORS ---

// A block of statements: { stmt1; stmt2; ... }
// Creates a new lexical scope so variables declared inside do not leak out.
// Called from: IfStmt branches, WhirlStmt body, ForeStmt body (inner),
//              DestinedStmt body, DivineStmt branch bodies.
//
// NOTE: SpellDecl bodies are NOT visited via BlockStmt — they are iterated
// directly in visit(SpellDecl*) which manages the spell scope explicitly.
void SemanticAnalyzer::visit(BlockStmt* node) {
    if (isPassOne) return;

    symbols.enterScope();
    for (auto& stmt : node->statements) {
        stmt->accept(*this);
    }
    symbols.exitScope();
}

// An expression used as a statement: process_data(data);   acknowledge_interrupt();
// Evaluates the expression for side effects (ownership moves, stance transitions
// triggered by evaluate() -> visit(CallExpr*), etc.).
// The resulting type is discarded — if you wanted the value you would have used
// an assignment, not a bare expression statement.
void SemanticAnalyzer::visit(ExprStmt* node) {
    if (isPassOne) return;
    evaluate(node->expression.get());
}

// yield (ctrl, data);   yield 0;   yield;
//
// Checks that the yield values are consistent with the enclosing spell's
// declared return type. For V1, we check:
//   - Void spells (abyss): yield with no values
//   - Single-value spells: yield with exactly one expression
//   - Tuple-returning spells: yield with the right number of values
//
// Full sub-type checking (e.g., "does the scroll match the Omen success type?")
// is a V1.5 improvement — the types are complex enough that getting the count
// right is the primary V1 safety check.
//
// WARDEN CONSTRAINT: warden spells must yield abyss. They cannot return values.
void SemanticAnalyzer::visit(YieldStmt* node) {
    if (isPassOne) return;
    if (!currentSpell) {
        error(node->token, "'yield' used outside of a spell body.");
    }

    // Warden spells must yield nothing (they are ISRs — they return to hardware).
    if (currentSpell->isWarden && !node->values.empty()) {
        error(node->token, "'warden spell' must yield 'abyss'. ISRs cannot return values.");
    }

    // Evaluate each yielded expression to type-check it and detect ownership moves.
    for (auto& val : node->values) {
        evaluate(val.get());
    }

    // V1.5 TODO: Check that yielded types match the spell's declared return type.
    // For now, the arity check (right number of values for tuple vs single return)
    // is the primary enforcement.
}

// shatter; — break out of the innermost loop.
// Error if used outside a fore or whirl loop.
void SemanticAnalyzer::visit(ShatterStmt* node) {
    if (isPassOne) return;
    if (loopDepth == 0) {
        error(node->token, "'shatter' used outside of a loop. Only valid inside 'fore' or 'whirl'.");
    }
}

// surge; — continue to the next loop iteration.
// Error if used outside a fore or whirl loop.
void SemanticAnalyzer::visit(SurgeStmt* node) {
    if (isPassOne) return;
    if (loopDepth == 0) {
        error(node->token, "'surge' used outside of a loop. Only valid inside 'fore' or 'whirl'.");
    }
}

// if (condition) { } elif (condition) { } else { }
// Checks that the condition produces a boolean-like type, then visits branches.
// Each branch (BlockStmt) manages its own scope.
void SemanticAnalyzer::visit(IfStmt* node) {
    if (isPassOne) return;

    // Evaluate and check the if condition.
    auto condType = evaluate(node->condition.get());
    // V1: We accept any type for boolean conditions (like C does).
    // V1.5 TODO: Enforce that condition is 'oath' or a comparable primitive.
    (void)condType;

    // Visit each branch. BlockStmt::accept() handles scope for each.
    node->thenBranch->accept(*this);

    for (auto& elifBranch : node->elifBranches) {
        auto elifCondType = evaluate(elifBranch.condition.get());
        (void)elifCondType;
        elifBranch.body->accept(*this);
    }

    if (node->elseBranch) {
        node->elseBranch->accept(*this);
    }
}

// fore (mark16 i = 0; i < 10; i++) { }
// Opens a scope for the loop variable, declares it, visits condition, body,
// and increment. Increments loopDepth so shatter/surge are valid inside.
void SemanticAnalyzer::visit(ForeStmt* node) {
    if (isPassOne) return;

    // Outer scope for the loop variable — it lives for the whole loop.
    symbols.enterScope();

    // Declare the loop variable: mark16 i = 0
    auto initType = resolveType(node->initType);
    symbols.declare(node->initVar.lexeme, initType);

    // Check the initializer value type (e.g., 0 is mark16-compatible).
    auto initValType = evaluate(node->initValue.get());
    (void)initValType; // V1.5 TODO: enforce compatible with initType

    // Check the condition (e.g., i < 10).
    auto condType = evaluate(node->condition.get());
    (void)condType;

    // Track loop nesting for shatter/surge validation.
    loopDepth++;

    // Visit the loop body. BlockStmt creates an inner scope for body statements.
    node->body->accept(*this);

    loopDepth--;

    // Check the increment expression (e.g., i = i + 1).
    if (node->incValue) {
        evaluate(node->incValue.get());
    }

    symbols.exitScope(); // Loop variable goes out of scope
}

// whirl (condition) { }
// Validates condition, increments loopDepth, visits body.
void SemanticAnalyzer::visit(WhirlStmt* node) {
    if (isPassOne) return;

    auto condType = evaluate(node->condition.get());
    (void)condType; // V1.5 TODO: enforce oath

    loopDepth++;
    node->body->accept(*this);
    loopDepth--;
}

// destined (condition) { cleanup_body; }
// destined { cleanup_body; }              <- condition is OPTIONAL
//
// The destined block is RAII cleanup that fires before every yield in the
// enclosing spell. During semantic analysis, we:
//   1. If a condition is present, evaluate it and check it is boolean-like.
//   2. Visit the body to check for errors inside the cleanup block.
//
// The actual CodeGen rewriting (every yield -> __ret = ...; goto __destined_N;)
// happens in the CodeGen phase, not here. Semantic analysis just validates.
//
// NOTE: destined does NOT create a new scope — it reads from the enclosing
// spell scope. The cleanup code needs to see ctrl, data, etc. from the spell.
void SemanticAnalyzer::visit(DestinedStmt* node) {
    if (isPassOne) return;
    if (!currentSpell) {
        error(node->token, "'destined' used outside of a spell body.");
    }

    // WARDEN CONSTRAINT: warden spells CAN use destined (per spec).
    // No restriction needed here.

    // Evaluate the optional condition.
    if (node->hasCondition) {
        auto condType = evaluate(node->condition.get());
        (void)condType; // V1.5 TODO: enforce boolean-like
    }

    // Visit the cleanup body. Note: NOT entering a new scope here.
    // The destined body shares the spell's scope — it needs to see all the
    // spell's variables (especially 'ctrl' for stance checks).
    for (auto& stmt : node->body->statements) {
        stmt->accept(*this);
    }
}

// my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
//     (ctrl, scroll data)                    => { ... }
//     (ctrl, ruin<DiskError::HardwareFault>) => { ... }
//     (ctrl, ruin err)                       => { ... }
// }
//
// This is the most complex semantic rule in the entire language. It handles:
//   1. The spell call must return an Omen type (T | ruin<R>).
//   2. The <~ rebinding must reference a real variable.
//   3. Each branch must be well-typed.
//   4. The block must be exhaustive.
//   5. OWNERSHIP RULE: my_disk is still owned-away INSIDE every branch.
//      The <~ rebinding happens AFTER the block completes, not inside branches.
//
// FIXED: The original code did targetSym->isMoved = false INSIDE the branch
//        loop, incorrectly resurreating ownership inside the block. Per spec:
//        "Inside branches, the caller's original variable is still owned-away."
//        The correct behavior is to restore isMoved to false AFTER all branches
//        have been analyzed.
void SemanticAnalyzer::visit(DivineStmt* node) {
    if (isPassOne) return;

    // 1. Evaluate the spell call. It must return a TUPLE.
    //    Then validate the tuple's second element is an Omen.
    //
    //    LANDMINE 3 FIX (Trojan Horse Tuple):
    //    Checking TypeKind::TUPLE alone is insufficient. A math spell returning
    //    (sigil* Disk, mark16) also produces a TUPLE, but its second element
    //    is mark16, not an Omen. divine on such a spell would compile here but
    //    crash in CodeGen when it emits __result.__elem1.__is_ruin on an int16_t.
    //
    //    We now check tupleElements[1].kind == TypeKind::OMEN explicitly.
    //    This requires tupleElements to be populated — which is why the CallExpr
    //    visitor now fills them in. Without that fix, this check would segfault.
    auto returnType = evaluate(node->spellCall.get());

    if (returnType->kind != TypeKind::TUPLE) {
        error(node->token,
              "'divine' requires a spell that returns a TUPLE: (sigil* Owner, T | ruin<R>). "
              "A spell returning a plain Omen (T | ruin<R>) should be unpacked with '?' instead.");
    }

    // Validate the tuple has at least two elements.
    if (returnType->tupleElements.size() < 2) {
        error(node->token,
              "'divine' requires a tuple with at least two elements: "
              "(sigil* Owner, T | ruin<R>). The spell's return tuple has fewer than 2 elements.");
    }

    // Validate the second element (index 1) is an Omen type.
    // This is the Trojan Horse check — catches (sigil* Disk, mark16) being used
    // with divine, which would produce __elem1.__is_ruin on a raw integer.
    if (returnType->tupleElements[1]->kind != TypeKind::OMEN) {
        error(node->token,
              "'divine' requires the second tuple element to be an Omen type (T | ruin<R>). "
              "The spell returns a tuple whose second element is '" +
              returnType->tupleElements[1]->name +
              "', which is not an Omen. Only tuple-returning spells with an Omen "
              "second element can be used with 'divine'.");
    }

    // 2. Find the target variable (e.g., my_disk).
    //    It must exist. It should currently be marked as moved (ownership was just
    //    transferred via the 'own &my_disk' argument in the spell call above).
    Symbol* targetSym = symbols.lookup(node->targetVar.lexeme);
    if (!targetSym) {
        error(node->targetVar, "Undeclared variable '" + node->targetVar.lexeme + "' in divine target.");
    }

    // 3. Analyze each branch.
    bool hasCatchAll = false;
    bool hasSuccess  = false;

    for (auto& branch : node->branches) {
        // Each branch gets its own scope for its local variables.
        symbols.enterScope();

        // Declare the ownership variable (ctrl).
        // IMPORTANT: 'ctrl' is a NEW local alias for the owned pointer INSIDE this branch.
        // The original variable (my_disk) is still moved and inaccessible — only 'ctrl'
        // is accessible inside the branch body.
        // The type of ctrl is the sigil pointer type (same type as my_disk).
        symbols.declare(
            branch.ownerVar.lexeme,
            targetSym->type,
            true,                      // isOwned=true (ctrl owns the pointer in this branch)
            targetSym->currentStance,  // carries the known stance from before the call
            false
        );

        if (branch.isRuin) {
            if (!branch.isSpecificRuin) {
                // Catch-all ruin: (ctrl, ruin err)
                hasCatchAll = true;
                auto errType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "soul16"});
                symbols.declare(branch.catchAllVar.lexeme, errType);
            } else {
                // Specific ruin: (ctrl, ruin<DiskError::HardwareFault>)
                if (!typeRegistry.count(branch.rankName.lexeme)) {
                    error(branch.rankName,
                          "Unknown rank type '" + branch.rankName.lexeme + "' in divine pattern.");
                }
            }
        } else if (branch.isPayloadless) {
            // Payloadless success branch: (ctrl) => { }
            // Used when the omen success type is 'abyss' — no payload to declare.
            // The body can reference ctrl and outer scope variables only.
            // DO NOT declare any payload variable here — that is the ghost variable trap.
            hasSuccess = true;
        } else {
            // Standard success branch: (ctrl, scroll data) => { ... }
            hasSuccess = true;
            auto successType = resolveType(branch.successType);
            symbols.declare(branch.successVar.lexeme, successType);
        }

        // Analyze the branch body.
        branch.body->accept(*this);

        symbols.exitScope();
    }

    // 4. Exhaustiveness check.
    //    Spec: "Must include exactly one success branch, plus exhaustive ruin coverage
    //    (enumerated variants OR a catch-all (ctrl, ruin err))."
    //
    //    FIXED: The original always required a catch-all. The spec allows specific-variant
    //    coverage without a catch-all. For V1, if all branches are specific ruins
    //    (no catch-all), we warn but don't error — full variant coverage checking
    //    requires enumerating the rank's variants, which is a V1.5 feature.
    if (!hasSuccess) {
        error(node->token, "Divine block is missing a success branch.");
    }
    if (!hasCatchAll) {
        // V1: Warn that variant coverage is not fully verified without a catch-all.
        // V1.5: Check that every variant of the ruin rank has a specific branch.
        std::cerr << "[Semantic Warning Line " << node->token.line << "] "
                  << "Divine block has no catch-all ruin branch. "
                  << "Full variant coverage is not verified in V1. "
                  << "Consider adding '(ctrl, ruin err) => { }' for safety.\n";
    }

    // 5. POST-BLOCK OWNERSHIP REBINDING (the <~ semantics).
    //    FIXED: This happens HERE, AFTER all branches are analyzed.
    //    The original code did this inside the branch loop — incorrect.
    //    Per spec: the caller's variable regains ownership after the divine
    //    block completes, not inside individual branches.
    if (targetSym) {
        targetSym->isMoved = false; // my_disk is accessible again
        // Stance becomes "Unknown" because different branches may have left
        // the controller in different stances (Idle from destined, Fault from error).
        // The programmer must perform a stance cast to use it with strict-stance spells.
        targetSym->currentStance = "Unknown";
    }
}

// Handles BOTH assignment statements AND stance transitions.
//
// NORMAL ASSIGNMENT: ctrl->sector_count = 5;
//   Left side is an expression (member access). Right side is evaluated.
//   V1.5 TODO: Full lvalue validation and type compatibility checking.
//
// STANCE TRANSITION: ctrl = Disk:Reading;
//   Left side is an identifier. Right side is a stance reference (IdentifierExpr
//   with stanceName set). The compiler updates the symbol's currentStance.
//   This is the moment the compiler "remembers" the hardware changed state.
//
// STANCE CAST: my_disk = Disk:Idle;
//   Same as above but after a divine block with Unknown stance.
//   Programmer is asserting the hardware is in a known state.
//   The compiler trusts this assertion and updates currentStance.
//
// FIXED: The original only handled IdentifierExpr on the left side.
//        Non-identifier left sides (ctrl->field = value) now fall through to
//        general expression evaluation rather than being silently ignored.
void SemanticAnalyzer::visit(AssignStmt* node) {
    if (isPassOne) return;

    // Check if the left side is a plain identifier.
    auto* targetIdent = dynamic_cast<IdentifierExpr*>(node->target.get());

    if (targetIdent) {
        Symbol* sym = symbols.lookup(targetIdent->token.lexeme);
        if (!sym) {
            error(targetIdent->token, "Assignment to undeclared variable '" + targetIdent->token.lexeme + "'.");
        }

        // Evaluate the right side.
        evaluate(node->value.get());

        // Check if the right side is a stance reference (Disk:Reading).
        auto* valIdent = dynamic_cast<IdentifierExpr*>(node->value.get());
        if (valIdent && !valIdent->stanceName.lexeme.empty()) {
            // This is a stance transition. Validate the sigil type matches.
            if (sym->type->kind != TypeKind::SIGIL &&
                sym->type->kind != TypeKind::HARDWARE) {
                error(node->token,
                      "Cannot perform stance transition on '" + sym->name +
                      "' — it is not a sigil.");
            }
            // Validate the sigil name matches (Disk:Reading on a Disk variable).
            if (sym->type->name != valIdent->token.lexeme) {
                error(node->token,
                      "Stance type mismatch: '" + sym->name + "' is '" + sym->type->name +
                      "' but stance belongs to '" + valIdent->token.lexeme + "'.");
            }
            // THE TYPESTATE UPDATE: The compiler now remembers the new stance.
            sym->currentStance = valIdent->stanceName.lexeme;
            return; // Stance transition complete — no further type checking needed.
        }

        // Standard assignment: type-check V1 (structural match not required for V1).
        // V1.5 TODO: check right type is assignment-compatible with left type.
        return;
    }

    // Left side is not a plain identifier (e.g., ctrl->field = value).
    // Evaluate both sides for side effects and ownership tracking.
    evaluate(node->target.get()); // Validates lvalue expression
    evaluate(node->value.get());  // Validates rvalue expression and type

    // V1.5 TODO: Validate that the target is a valid lvalue.
    // V1.5 TODO: Validate direct writes to __stance are a compile error.
}

void SemanticAnalyzer::visit(VarDeclStmt* node) {
    if (isPassOne) return;

    auto varType = resolveType(node->typeToken);

    std::string initialStance = "";

    if (node->initializer) {
        auto initType = evaluate(node->initializer.get());
        
        // THE FIX: If initialized with a stance-prefixed struct, remember the stance!
        if (auto* structInit = dynamic_cast<StructInitExpr*>(node->initializer.get())) {
            initialStance = structInit->stanceName.lexeme;
        }
    }

    // Register it in the current scope with the correct initial stance
    symbols.declare(
        node->name.lexeme, 
        varType, 
        false, 
        initialStance, 
        false
    );
}

// =============================================================================
// EXPRESSION VISITORS
// =============================================================================

// Binary infix operations: a + b, a == b, ctrl->stance, a ~> b, etc.
//
// For member access (-> and .): left side must be a sigil type,
//                               right side is a field name identifier.
//   Result type = the field's type (looked up from the sigil's fields).
//   For ->stance access: returns soul16 (the __stance discriminant).
//   V1: We return the left side's type as a simplified approximation.
//
// For pipeline (~>): left output type feeds into right as first arg.
//   Result type = the right spell's return type.
//   V1: We evaluate both sides and return the right side's type.
//
// For arithmetic and comparison: V1 returns a primitive type.
void SemanticAnalyzer::visit(BinaryExpr* node) {
    if (isPassOne) return;

    auto leftType = evaluate(node->left.get());
    std::shared_ptr<TypeInfo> rightType = nullptr;

    // THE FIX: Do not evaluate the right side if this is member access!
    // For 'a->b' or 'a.b', 'b' is a field name, not a local variable.
    if (node->op.type != TokenType::ARROW && node->op.type != TokenType::DOT) {
        rightType = evaluate(node->right.get());
    }

    switch (node->op.type) {
        case TokenType::ARROW:
        case TokenType::DOT:
            // Member access: ctrl->stance, node.sector_count
            // V1: Return left side's type. V1.5: look up the field type.
            // The critical check: ->stance is read-only (no write allowed).
            // That enforcement is in visit(AssignStmt*) which checks targets.
            currentExprType = leftType;
            break;

        case TokenType::WEAVE:
            // ~> pipeline: left output feeds into right as first arg.
            // The result type is whatever the right-hand call returns.
            // Since evaluate(right) already set currentExprType, it's correct.
            currentExprType = rightType;
            break;

        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::GT:
        case TokenType::LT:
            // Comparisons always produce a boolean-like result.
            currentExprType = typeRegistry["oath"];
            break;

        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
            // Arithmetic: result type is the left operand's type.
            // V1.5 TODO: enforce both sides are numeric.
            currentExprType = leftType;
            break;

        case TokenType::PIPE:
            // | in expression context = Omen union (T | ruin<R>).
            // V1: This appears mainly in type annotations, not as a runtime
            // expression. We return an OMEN type.
            currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
            break;

        default:
            // Unknown binary operator — pass through left type.
            currentExprType = leftType;
            break;
    }
}

// Prefix unary operations: -x (negation), *ptr (pointer dereference).
void SemanticAnalyzer::visit(UnaryExpr* node) {
    if (isPassOne) return;

    auto operandType = evaluate(node->operand.get());

    if (node->op.type == TokenType::STAR) {
        // Pointer dereference *ptr
        // V1.5 TODO: Validate operand is a pointer type, return pointee type.
        currentExprType = operandType; // Simplified
    } else {
        // Negation or other prefix op — same type as operand.
        currentExprType = operandType;
    }
}

// Postfix '?' — the Omen unpack operator.
// e.g., read_buffer()?
//
// SEMANTICS: The operand must be an Omen type (T | ruin<R>).
//   On success: unwraps and produces the success value T.
//   On ruin: immediately yields the ruin up the call chain.
//
// For the semantic analyzer, we:
//   1. Evaluate the operand and confirm it's an Omen.
//   2. Set currentExprType to the Omen's successType (the unwrapped value).
//
// WARDEN CONSTRAINT: warden spells cannot propagate ruin.
//   If ? is used inside a warden spell, it would propagate a ruin to hardware
//   context, which is forbidden. V1.5 TODO: enforce this.
void SemanticAnalyzer::visit(PostfixExpr* node) {
    if (isPassOne) return;

    auto operandType = evaluate(node->operand.get());

    if (operandType->kind != TypeKind::OMEN) {
        error(node->op,
              "'?' can only be applied to an Omen type (T | ruin<R>). "
              "The expression before '?' does not return an Omen.");
    }

    // RESOLVEDOMENTYPE PATCH:
    // Store the full Omen TypeInfo on the AST node so CodeGen can:
    //   a) Emit the correct concrete typedef name for _tmp (not __auto_type fallback)
    //   b) Detect abyss Omens where __value does not exist in the union
    //   c) Emit the correct early-return path in the destined goto chain
    // This is the fix for the Type Erasure bug identified in the pre-Phase-3 audit.
    node->resolvedOmenType = operandType;

    // Unwrap: the ? operator produces the SUCCESS type of the Omen.
    if (operandType->successType) {
        currentExprType = operandType->successType;
    } else {
        // Omen with no successType populated — produce a generic primitive.
        // This happens when the spell's return type was not fully resolved.
        // V1.5 TODO: Ensure all Omen types are fully populated with successType.
        currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "soul16"});
    }
}

// An integer literal, string literal, or boolean literal.
// The type is determined by the token type.
//
//   INT_LIT    -> mark16 (default integer per spec; widened at use site if needed)
//   STRING_LIT -> scroll (fat pointer; len calculated by CodeGen)
//   KEPT       -> oath (true)
//   FORSAKEN   -> oath (false)
void SemanticAnalyzer::visit(LiteralExpr* node) {
    if (isPassOne) return;

    switch (node->token.type) {
        case TokenType::INT_LIT:
            // Integer literals default to mark16 per spec.
            // The CodeGen and type-checking will widen or cast as needed.
            currentExprType = typeRegistry["mark16"];
            break;

        case TokenType::STRING_LIT:
            // String literals are Cgil_Scroll values.
            currentExprType = typeRegistry["scroll"];
            break;

        case TokenType::KEPT:
        case TokenType::FORSAKEN:
            // Boolean literals.
            currentExprType = typeRegistry["oath"];
            break;

        default:
            // Fallback for any other literal-like token.
            currentExprType = typeRegistry["mark16"];
            break;
    }
}

// A reference to a named variable, stance, or rank variant.
//
// Three cases based on what stanceName and variantName contain:
//
//   PLAIN IDENTIFIER (stanceName empty, variantName empty):
//     Normal variable lookup. Check it exists. Check it's not moved.
//     Return its type.
//
//   STANCE REFERENCE (stanceName set): e.g., Disk:Fault
//     The sigil type name must exist. Returns a special __STANCE_REF__ pseudo-type
//     so AssignStmt knows to treat this as a stance transition.
//
//   RANK VARIANT REFERENCE (variantName set): e.g., DiskError::Timeout
//     The rank type must exist. Returns a soul16 (the discriminant value).
//     V1.5 TODO: Validate the variant name is actually in that rank.
void SemanticAnalyzer::visit(IdentifierExpr* node) {
    if (isPassOne) return;

    // CASE 1: Stance reference (Disk:Fault)
    if (!node->stanceName.lexeme.empty()) {
        auto it = typeRegistry.find(node->token.lexeme);
        if (it == typeRegistry.end() || it->second->kind != TypeKind::SIGIL) {
            error(node->token,
                  "'" + node->token.lexeme + "' is not a known sigil type. "
                  "Stance references must use a declared sigil name.");
        }
        // Return a sentinel type so AssignStmt can recognize a stance transition.
        // The sentinel name encodes which sigil and which stance.
        auto stanceRefType = std::make_shared<TypeInfo>(TypeInfo{
            TypeKind::PRIMITIVE,
            "__STANCE_REF__:" + node->token.lexeme + ":" + node->stanceName.lexeme
        });
        currentExprType = stanceRefType;
        return;
    }

    // CASE 2: Rank variant reference (DiskError::Timeout)
    if (!node->variantName.lexeme.empty()) {
        auto it = typeRegistry.find(node->token.lexeme);
        if (it == typeRegistry.end() || it->second->kind != TypeKind::RANK) {
            error(node->token,
                  "'" + node->token.lexeme + "' is not a known rank type. "
                  "Variant access (::) requires a declared rank name.");
        }
        // V1.5 TODO: Validate node->variantName.lexeme is in the rank's variants.
        // Rank variants are soul16 discriminants at runtime.
        currentExprType = typeRegistry["soul16"];
        return;
    }

    // CASE 3: Plain variable lookup
    Symbol* sym = symbols.lookup(node->token.lexeme);
    if (!sym) {
        error(node->token, "Undeclared identifier '" + node->token.lexeme + "'.");
    }

    // --- THE OWNERSHIP SAFETY LOCK ---
    // If this variable had ownership transferred away (passed with 'own'),
    // using it again is a compile error. The programmer must wait for
    // <~ to rebind it after a divine block.
    if (sym->isOwned && sym->isMoved) {
        error(node->token,
              "Use-after-move: '" + sym->name + "' has been transferred with 'own' "
              "and cannot be used until ownership is rebound via '<~'.");
    }

    currentExprType = sym->type;
}

// A function call: fetch_sector(own &my_disk, 0x0500)
//                 ruin(DiskError::HardwareFault)
//
// Checks:
//   1. The callee is a known spell.
//   2. The argument count matches the parameter count.
//   3. For sigil* parameters with stance constraints: the argument's current
//      stance matches the required stance (THE TYPESTATE LOCK).
//   4. For 'own' parameters: the 'own' keyword was present at the call site.
//      The argument variable is then marked as moved (isMoved = true).
//   5. For warden spells: own and ruin propagation are forbidden.
//
// FIXED: Added handling for ruin(...) call expressions, which appear in yield
//        statements as error value construction. These are NOT spell calls —
//        they are special syntax for building Omen error values.
void SemanticAnalyzer::visit(CallExpr* node) {
    if (isPassOne) return;

    auto* calleeIdent = dynamic_cast<IdentifierExpr*>(node->callee.get());
    if (!calleeIdent) {
        error(node->token, "Only named spells can be called in V1.");
    }

    const std::string& calleeName = calleeIdent->token.lexeme;

    // SPECIAL CASE: ruin(DiskError::HardwareFault)
    // This is not a spell call — it's an Omen error construction expression.
    // It takes one argument (a rank variant) and returns an Omen type.
    // It appears in: yield (ctrl, ruin(DiskError::HardwareFault));
    if (calleeName == "ruin") {
        if (node->args.size() != 1) {
            error(node->token, "'ruin(...)' takes exactly one rank variant argument.");
        }
        // Evaluate the argument (the rank variant expression).
        evaluate(node->args[0].get());
        // Produces an Omen type.
        currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
        return;
    }

    // Normal spell lookup.
    auto spellIt = spellRegistry.find(calleeName);
    if (spellIt == spellRegistry.end()) {
        error(node->token, "Call to unknown spell '" + calleeName + "'.");
    }
    SpellDecl* spell = spellIt->second;

    // WARDEN CONSTRAINT: warden spells cannot use 'own' (per spec).
    if (currentSpell && currentSpell->isWarden) {
        for (size_t i = 0; i < node->argIsOwned.size(); i++) {
            if (node->argIsOwned[i]) {
                error(node->args[i]->token,
                      "'warden spell' cannot use 'own' — ISRs cannot transfer hardware ownership.");
            }
        }
    }

    // Arity check.
    if (node->args.size() != spell->params.size()) {
        error(node->token,
              "Argument count mismatch. Spell '" + calleeName + "' expects " +
              std::to_string(spell->params.size()) + " arguments, got " +
              std::to_string(node->args.size()) + ".");
    }

    // Per-argument checks.
    for (size_t i = 0; i < node->args.size(); i++) {
        auto argType  = evaluate(node->args[i].get());
        Param& param  = spell->params[i];

        if (param.isPointer && !param.stanceName.lexeme.empty()) {
            // This parameter requires the argument to be in a specific stance.
            // We must unwrap AddressOfExpr (&my_disk) to find the actual variable.
            
            auto* argIdent = dynamic_cast<IdentifierExpr*>(node->args[i].get());
            auto* addrOf = dynamic_cast<AddressOfExpr*>(node->args[i].get());
            
            // If it's &my_disk, look inside to grab the 'my_disk' identifier
            if (addrOf) {
                argIdent = dynamic_cast<IdentifierExpr*>(addrOf->operand.get());
            }

            // NOW we check if we successfully found an identifier
            if (!argIdent) {
                error(node->args[i]->token,
                      "Parameter '" + param.name.lexeme + "' requires a sigil variable "
                      "(not a complex expression) so its stance can be tracked.");
            }

            Symbol* argSym = symbols.lookup(argIdent->token.lexeme);

            if (!argSym) {
                error(node->args[i]->token, "Cannot resolve argument variable for stance check.");
            }

            // --- THE TYPESTATE LOCK ---
            if (argSym->currentStance != param.stanceName.lexeme) {
                error(node->args[i]->token,
                      "Stance mismatch: spell '" + calleeName + "' requires '" +
                      param.stanceName.lexeme + "' but '" + argSym->name +
                      "' is currently '" + argSym->currentStance + "'. "
                      "Cannot call this spell on hardware in the wrong state.");
            }

            // --- THE OWNERSHIP LOCK ---
            if (param.isOwned) {
                if (!node->argIsOwned[i]) {
                    error(node->args[i]->token,
                          "Parameter '" + param.name.lexeme + "' requires 'own'. "
                          "Write: fetch_sector(own &my_disk, ...) to acknowledge the ownership transfer.");
                }
                // Transfer ownership. The variable is now inaccessible until <~ rebinds it.
                argSym->isMoved = true;
            }
        }
    }

    // Determine the return type.
    // CRITICAL (Landmine 3 fix + Type Erasure fix):
    //
    //   TUPLE return (multiple elements):
    //     Produces TypeKind::TUPLE with tupleElements populated.
    //     tupleElements[0] = resolved type of first return element
    //     tupleElements[1] = resolved type of second return element (usually OMEN)
    //     Downstream passes (DivineStmt) NEED these elements to be populated.
    //     An empty tupleElements is type erasure — the validator is blinded.
    //
    //   OMEN return (single value with ruin suffix):
    //     Produces TypeKind::OMEN. Used by '?' postfix unpack.
    //
    //   Single primitive return:
    //     Produces the resolved primitive TypeInfo.
    //
    //   abyss return (no values):
    //     Produces the abyss TypeInfo.
    if (spell->returnTypes.size() > 1) {
        // Multi-element tuple return.
        auto tupleType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::TUPLE, "Tuple"});

        // Populate tupleElements so downstream validators can inspect them.
        // Without this, visit(DivineStmt) cannot verify __elem1 is actually an Omen.
        for (const auto& rt : spell->returnTypes) {
            auto elemIt = typeRegistry.find(rt.typeToken.lexeme);
            if (elemIt != typeRegistry.end()) {
                tupleType->tupleElements.push_back(elemIt->second);
            } else {
                // Unknown element type — push a placeholder so indexing is safe.
                tupleType->tupleElements.push_back(
                    std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, rt.typeToken.lexeme})
                );
            }
        }

        // If the spell has an Omen suffix, replace the last element type with
        // an OMEN TypeInfo (the omen applies to the last listed return type).
        if (spell->hasOmen && !tupleType->tupleElements.empty()) {
            auto omenType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
            tupleType->tupleElements.back() = omenType;
        }

        currentExprType = tupleType;

    } else if (spell->hasOmen) {
        // Single-element Omen: scroll | ruin<E>
        currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});

    } else if (!spell->returnTypes.empty()) {
        auto retIt = typeRegistry.find(spell->returnTypes[0].typeToken.lexeme);
        currentExprType = (retIt != typeRegistry.end())
            ? retIt->second
            : typeRegistry["abyss"];

    } else {
        currentExprType = typeRegistry["abyss"];
    }
}

// The address-of operator: &my_disk, &disk_data_port
//
// TWO COMPLETELY DIFFERENT SEMANTICS depending on what the operand is:
//
//   &regular_variable -> C address-of. Returns a pointer to the variable.
//                        Type: a pointer to the variable's type.
//
//   &hardware_variable -> Compile-time address constant. Returns the physical
//                         address of the leyline/portline as an 'addr' value.
//                         e.g., &disk_data_port evaluates to 0x1F0 as addr.
//                         This is NOT a pointer dereference — it is a constant.
//                         CodeGen emits: (uint16_t)0x1F0
//
// The isHardware flag on the Symbol (set by HardwareDecl) is what enables
// this distinction.
void SemanticAnalyzer::visit(AddressOfExpr* node) {
    if (isPassOne) return;

    // Evaluate the operand to resolve what it refers to.
    auto operandType = evaluate(node->operand.get());

    // Check if the operand is an identifier referring to a hardware variable.
    auto* ident = dynamic_cast<IdentifierExpr*>(node->operand.get());
    if (ident) {
        Symbol* sym = symbols.lookup(ident->token.lexeme);
        if (sym && sym->isHardware) {
            // Hardware address-of: &portline_name -> compile-time addr constant.
            // CodeGen will emit: ((uint16_t)0xADDR) not &variable.
            currentExprType = typeRegistry["addr"];
            return;
        }
    }

    // Regular address-of: &my_disk -> pointer to the variable.
    // V1: Return the operand's type. V1.5: wrap in a pointer TypeInfo.
    currentExprType = operandType;
}

// Array subscript: target[index]
// Validates:
//   1. The target is an array-like type (deck variable, leyline of array type).
//   2. The index is an integer-compatible type.
//   3. Returns the element type of the array.
//
// V1: Type checking is simplified — we evaluate both sides and return mark16.
// V1.5 TODO: Track element types on array TypeInfos and return the correct type.
void SemanticAnalyzer::visit(IndexExpr* node) {
    if (isPassOne) return;

    auto targetType = evaluate(node->target.get());
    auto indexType  = evaluate(node->index.get());

    // V1: Accept any index type (like C does for array subscripts).
    // V1.5 TODO: Enforce index is mark16, soul16, or addr.
    (void)indexType;

    // V1: Return mark16 as the element type — conservative safe default.
    // V1.5 TODO: Return targetType->elementType when array TypeInfo carries it.
    currentExprType = typeRegistry.count("mark16")
        ? typeRegistry["mark16"]
        : std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "mark16"});
}

// Struct/sigil initializer: Device:Idle { device_id: 0, error_code: 0, flags: 0 }
//
// Validates:
//   1. The type name refers to a declared sigil or legion.
//   2. If a stance prefix is present, the stance belongs to that sigil.
//   3. All field names exist on the sigil (V1.5 TODO).
//   4. All field values evaluate without error.
//
// Produces: the sigil's TypeInfo as the expression type.
void SemanticAnalyzer::visit(StructInitExpr* node) {
    if (isPassOne) return;

    // 1. Resolve the type name.
    auto typeIt = typeRegistry.find(node->typeName.lexeme);
    if (typeIt == typeRegistry.end()) {
        error(node->typeName,
              "Unknown type '" + node->typeName.lexeme + "' in struct initializer. "
              "Only declared sigil and legion types can be initialized with { }.");
    }

    auto sigilType = typeIt->second;

    if (sigilType->kind != TypeKind::SIGIL) {
        error(node->typeName,
              "'" + node->typeName.lexeme + "' is not a sigil or legion type. "
              "Struct initializer { } syntax is only valid for sigil and legion types.");
    }

    // 2. Validate stance prefix if present.
    if (!node->stanceName.lexeme.empty()) {
        // V1: We trust the stance name is valid (Parser consumed it as an IDENT).
        // V1.5 TODO: Look up the stanceMap for this sigil and verify the stance exists.
    }

    // 3. Evaluate all field initializer expressions.
    // V1: We don't validate field names against the sigil definition (V1.5 TODO).
    // We do evaluate initializers to catch type errors and ownership moves inside them.
    for (auto& field : node->fields) {
        evaluate(field.value.get());
    }

    // The expression type is the sigil type itself.
    currentExprType = sigilType;
}