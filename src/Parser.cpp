#include "Parser.h"
#include <iostream>

// =============================================================================
// CONSTRUCTOR AND MAIN ENTRY POINT
// =============================================================================

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

// parse() drives the top-level loop. It calls parseDeclaration() for each
// top-level construct in the file. If parseDeclaration() throws (a parse error),
// the error is printed and synchronize() skips to the next safe recovery point
// so parsing can continue. This means one run can report multiple errors.
std::vector<std::unique_ptr<Decl>> Parser::parse() {
    std::vector<std::unique_ptr<Decl>> declarations;

    while (!isAtEnd()) {
        try {
            declarations.push_back(parseDeclaration());
        } catch (const std::runtime_error& e) {
            std::cerr << e.what() << "\n";
            synchronize();
        }
    }

    return declarations;
}

// =============================================================================
// DECLARATION PARSERS
// =============================================================================

// Routes to the right declaration parser based on the keyword we see.
// FIXED: Now handles rank, legion, grimoire, and pact — these were missing
// from the original and would have caused a parse error on every .gil file
// that used them (which is every file in the spec example).
std::unique_ptr<Decl> Parser::parseDeclaration() {
    if (match({TokenType::GRIMOIRE, TokenType::PACT}))
        return parseGrimoireDecl();
    if (match({TokenType::RANK}))
        return parseRankDecl();
    if (match({TokenType::SIGIL}))
        return parseSigilDecl();
    if (match({TokenType::LEGION}))
        return parseLegionDecl();
    if (match({TokenType::LEYLINE, TokenType::PORTLINE}))
        return parseHardwareDecl();
    if (match({TokenType::SPELL, TokenType::WARDEN, TokenType::CONJURE}))
        return parseSpellDecl(true);

    error(peek(), "Expected a top-level declaration. Valid keywords: "
                  "spell, warden, conjure, sigil, rank, legion, "
                  "leyline, portline, grimoire, pact.");
}

// grimoire <hardware_defs.h>;
// pact <stdlib.h>;
//
// The previous() token on entry is GRIMOIRE or PACT (consumed by match()).
// isPact distinguishes them for the semantic analyzer (pact = Ring 3 warning).
//
// NOTE: For V1, header paths like "hardware_defs.h" are lexed as
// IDENT DOT IDENT. We consume only the first IDENT for simplicity.
// A production-quality implementation would collect all tokens up to '>'.
std::unique_ptr<GrimoireDecl> Parser::parseGrimoireDecl() {
    auto node = std::make_unique<GrimoireDecl>();
    node->token  = previous();                                // grimoire or pact token
    node->isPact = (previous().type == TokenType::PACT);

    consume(TokenType::LT, "Expected '<' after grimoire/pact.");

    // THE FIX: Eat tokens and stitch them together until we hit '>'
    std::string pathStr = "";
    while (!check(TokenType::GT) && !isAtEnd()) {
        pathStr += advance().lexeme;
    }

    // Check if they left it empty (e.g., grimoire <>;)
    if (pathStr.empty()) {
        throw std::runtime_error("[Parse Error] Expected header file name between < >");
    }

    // Synthesize a single Token to hold the full stitched path
    Token pathToken = previous(); // Use the last token for line/col info
    pathToken.lexeme = pathStr;   // E.g., "stdio.h" or "sys/types.h"
    node->path = pathToken;

    consume(TokenType::GT, "Expected '>' after header name.");
    consume(TokenType::SEMICOLON, "Expected ';' after grimoire declaration.");

    return node;
}

// rank DiskError { Timeout, HardwareFault, InvalidSector }
//
// Variants are stored in declaration order. The semantic analyzer assigns
// sequential uint16_t discriminants: Timeout=0, HardwareFault=1, etc.
// The CodeGen emits typedef + #define constants for each variant.
std::unique_ptr<RankDecl> Parser::parseRankDecl() {
    auto node = std::make_unique<RankDecl>();
    node->token = previous(); // 'rank' token

    node->name = consume(TokenType::IDENT, "Expected rank name after 'rank'.");
    consume(TokenType::LBRACE, "Expected '{' before rank body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->variants.push_back(consume(TokenType::IDENT, "Expected variant name."));
        if (!match({TokenType::COMMA})) break; // Trailing comma is optional
    }

    consume(TokenType::RBRACE, "Expected '}' after rank variants.");
    return node;
}

// sigil Disk { stance Idle; stance Reading; soul16 sector_count; }
//
// Inside the sigil body we can encounter:
//   - 'stance' keyword  -> a typestate declaration
//   - 'spell' keyword   -> a bound method (V1.5 encapsulation feature)
//   - anything else     -> a data field (type + name)
//
// Stances and fields are stored in declaration order because the CodeGen must
// emit them in that order: __stance first (implicit), then fields in order.
std::unique_ptr<SigilDecl> Parser::parseSigilDecl() {
    auto node = std::make_unique<SigilDecl>();
    node->token = previous(); // 'sigil' token

    node->name = consume(TokenType::IDENT, "Expected sigil name after 'sigil'.");
    consume(TokenType::LBRACE, "Expected '{' before sigil body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match({TokenType::STANCE})) {
            // stance Idle;
            node->stances.push_back(consume(TokenType::IDENT, "Expected stance name after 'stance'."));
            consume(TokenType::SEMICOLON, "Expected ';' after stance declaration.");

        } else if (match({TokenType::SPELL})) {
            // Bound spell — a method attached to this sigil (V1.5 feature).
            // isTopLevel=false signals to parseSpellDecl that this spell belongs
            // to a sigil and should not be treated as a standalone function.
            node->boundSpells.push_back(parseSpellDecl(false));

        } else {
            // Data field: soul16 sector_count;
            Param field;
            field.isOwned   = false;
            field.isPointer = false;
            field.type      = consumeType("Expected a valid type for sigil field.");
            field.name      = consume(TokenType::IDENT, "Expected field name.");
            consume(TokenType::SEMICOLON, "Expected ';' after field declaration.");
            node->fields.push_back(field);
        }
    }

    consume(TokenType::RBRACE, "Expected '}' after sigil body.");
    return node;
}

// legion SectorCache { mark16 sector_id; flow read_time; oath is_corrupted; }
//
// Written like a sigil with no stances. V1: Parsed distinctly but stubbed as
// a sigil in CodeGen. V2 will implement the full SoA memory layout.
std::unique_ptr<LegionDecl> Parser::parseLegionDecl() {
    auto node = std::make_unique<LegionDecl>();
    node->token = previous(); // 'legion' token

    node->name = consume(TokenType::IDENT, "Expected legion name after 'legion'.");
    consume(TokenType::LBRACE, "Expected '{' before legion body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Param field;
        field.type = consumeType("Expected a valid type for legion field.");
        field.name = consume(TokenType::IDENT, "Expected field name.");
        consume(TokenType::SEMICOLON, "Expected ';' after field declaration.");
        node->fields.push_back(field);
    }

    consume(TokenType::RBRACE, "Expected '}' after legion body.");
    return node;
}

// Parses all four spell variants. The 'previous()' token on entry is
// SPELL, WARDEN, or CONJURE (consumed by match() in parseDeclaration or
// by the explicit match in parseSigilDecl for bound spells).
//
// isTopLevel: true  = standalone spell at file scope
//             false = bound spell inside a sigil body
//
// FIXED: Default argument (bool isTopLevel = true) is in the DECLARATION
// (Parser.h), not in this definition. In C++, default arguments must be
// in the declaration, not the definition. Having it here previously caused
// a signature mismatch that prevented compilation.
std::unique_ptr<SpellDecl> Parser::parseSpellDecl(bool isTopLevel) {
    auto node = std::make_unique<SpellDecl>();
    node->token = previous();

    node->isWarden  = (node->token.type == TokenType::WARDEN);
    node->isConjure = (node->token.type == TokenType::CONJURE);

    // After 'conjure', check for optional 'endless' modifier.
    if (node->isConjure && match({TokenType::ENDLESS})) {
        node->isEndless = true;
    }

    // 'warden' and 'conjure' are modifiers — the 'spell' keyword still follows.
    if (node->isWarden || node->isConjure) {
        consume(TokenType::SPELL, "Expected 'spell' keyword after modifier.");
    }

    node->name = consume(TokenType::IDENT, "Expected spell name.");
    consume(TokenType::LPAREN, "Expected '(' after spell name.");

    // --- Parameters ---
    if (!check(TokenType::RPAREN)) {
        do {
            Param p;
            p.isOwned = match({TokenType::OWN}); // 'own' is optional

            if (match({TokenType::SIGIL})) {
                // Sigil pointer parameter: [own] sigil* TypeName[:StanceName] paramName
                consume(TokenType::STAR, "Expected '*' after 'sigil' in parameter.");
                p.type      = consume(TokenType::IDENT, "Expected sigil type name.");
                p.isPointer = true;

                // Optional stance constraint: Disk:Idle
                if (match({TokenType::COLON})) {
                    p.stanceName = consume(TokenType::IDENT, "Expected stance name after ':'.");
                }
            } else {
                // Primitive type: addr target, scroll msg, mark16 x, etc.
                p.type      = consumeType("Expected a valid parameter type.");
                p.isPointer = false;
            }

            p.name = consume(TokenType::IDENT, "Expected parameter name.");
            node->params.push_back(p);

        } while (match({TokenType::COMMA}));
    }
    consume(TokenType::RPAREN, "Expected ')' after parameter list.");

    // --- Return Type ---
    consume(TokenType::ARROW, "Expected '->' before return type.");

    if (match({TokenType::LPAREN})) {
        // Tuple return: (sigil* Disk, scroll | ruin<DiskError>)
        // FIXED (Landmine 2): Each element is stored as ReturnTypeInfo carrying
        // both the type token AND the isPointer flag. The Parser no longer discards '*'.
        do {
            ReturnTypeInfo ri;
            if (match({TokenType::SIGIL})) {
                // sigil* TypeName — consume the * and record isPointer=true
                consume(TokenType::STAR, "Expected '*' after 'sigil' in return type.");
                ri.typeToken  = consumeType("Expected sigil type name after 'sigil*'.");
                ri.isPointer  = true;
            } else {
                ri.typeToken  = consumeType("Expected a valid return type.");
                ri.isPointer  = false;
            }
            node->returnTypes.push_back(ri);
        } while (match({TokenType::COMMA}));
        consume(TokenType::RPAREN, "Expected ')' after tuple return type.");
    } else {
        // Single return type: scroll, mark16, abyss, etc.
        ReturnTypeInfo ri;
        ri.typeToken = consumeType("Expected a valid return type.");
        ri.isPointer = false;
        node->returnTypes.push_back(ri);
    }

    // Optional Omen: | ruin<DiskError>
    if (match({TokenType::PIPE})) {
        consume(TokenType::RUIN, "Expected 'ruin' after '|' in return type.");
        consume(TokenType::LT,   "Expected '<' after 'ruin'.");
        node->omenErrorType = consume(TokenType::IDENT, "Expected rank name inside ruin<...>.");
        consume(TokenType::GT,   "Expected '>' after ruin rank name.");
        node->hasOmen = true;
    }

    // conjure spells are extern declarations — no body.
    if (node->isConjure) {
        consume(TokenType::SEMICOLON, "Expected ';' after conjure declaration.");
        return node;
    }

    // --- Body ---
    consume(TokenType::LBRACE, "Expected '{' before spell body.");
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->body.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}' after spell body.");

    return node;
}

// leyline disk_status_port: rune @ 0x1F7;
// portline disk_data_port: soul16 @ 0x1F0;
//
// isPortline=false -> leyline (MMIO) -> volatile pointer in CodeGen
// isPortline=true  -> portline (PIO) -> inb/outb inline assembly in CodeGen
std::unique_ptr<HardwareDecl> Parser::parseHardwareDecl() {
    auto node = std::make_unique<HardwareDecl>();
    node->token      = previous(); // 'leyline' or 'portline' token
    node->isPortline = (node->token.type == TokenType::PORTLINE);

    node->name    = consume(TokenType::IDENT,    "Expected identifier after hardware keyword.");
    consume(TokenType::COLON, "Expected ':' after hardware name.");
    node->type    = consumeType("Expected a valid type for hardware declaration (e.g., rune, soul16).");
    consume(TokenType::AT,    "Expected '@' before hardware address.");
    node->address = consume(TokenType::INT_LIT,  "Expected address literal after '@'.");
    consume(TokenType::SEMICOLON, "Expected ';' after hardware declaration.");

    return node;
}

// =============================================================================
// STATEMENT PARSERS
// =============================================================================

// Routes to the correct statement parser. Falls through to parseExprOrAssignStmt
// for everything that does not start with a recognized keyword.
std::unique_ptr<Stmt> Parser::parseStatement() {
    // Intercept Primitive Types
    if (check(TokenType::MARK16) || check(TokenType::MARK32) ||
        check(TokenType::SOUL16) || check(TokenType::SOUL32) ||
        check(TokenType::ADDR)   || check(TokenType::FLOW)   ||
        check(TokenType::RUNE)   || check(TokenType::OATH)   ||
        check(TokenType::SCROLL) || check(TokenType::ABYSS)  ||
        check(TokenType::DECK)) {
        Token typeToken = advance();
        return parseVarDeclStmt(typeToken);
    }

    // Intercept Sigil/Legion local declarations (e.g., sigil Device my_dev = ...)
    if (match({TokenType::SIGIL, TokenType::LEGION})) {
        Token typeToken = consume(TokenType::IDENT, "Expected type name after sigil/legion.");
        return parseVarDeclStmt(typeToken);
    }
    
    if (match({TokenType::IF}))       return parseIfStmt();
    if (match({TokenType::FORE}))     return parseForeStmt();
    if (match({TokenType::WHIRL}))    return parseWhirlStmt();
    if (match({TokenType::YIELD}))    return parseYieldStmt();
    if (match({TokenType::DESTINED})) return parseDestinedStmt();

    if (match({TokenType::SHATTER})) {
        auto node = std::make_unique<ShatterStmt>();
        node->token = previous();
        consume(TokenType::SEMICOLON, "Expected ';' after 'shatter'.");
        return node;
    }

    if (match({TokenType::SURGE})) {
        auto node = std::make_unique<SurgeStmt>();
        node->token = previous();
        consume(TokenType::SEMICOLON, "Expected ';' after 'surge'.");
        return node;
    }

    if (match({TokenType::LBRACE})) {
        // A bare block statement. '{' was just consumed by match().
        // parseBlock() takes over from the next token onward.
        return parseBlock();
    }

    // Detect the divine pattern: IDENT <~ divine ...
    // This requires looking two tokens ahead:
    //   peek()            = IDENT    (the target variable, e.g., my_disk)
    //   peekNext()        = REV_WEAVE  (<~)
    //   tokens[current+2] = DIVINE
    //
    // FIXED: Added bounds check (current + 2 < tokens.size()) before accessing
    // tokens[current + 2]. Without this, accessing past the end of the vector
    // is undefined behavior and can crash the compiler.
    if (check(TokenType::IDENT) &&
        peekNext().type == TokenType::REV_WEAVE &&
        (current + 2 < (int)tokens.size()) &&
        tokens[current + 2].type == TokenType::DIVINE) {
        return parseDivineStmt();
    }

    return parseExprOrAssignStmt();
}

// Parses the BODY of a block — the contents between { and }.
//
// CONVENTION: The opening '{' must ALREADY be consumed by the caller before
// calling parseBlock(). This is consistent throughout the parser:
//
//   consume(TokenType::LBRACE, "...");  // caller's job
//   node->body = parseBlock();          // parseBlock reads until } and consumes it
//
// parseBlock reads statements until it sees '}', then consumes '}'.
// The caller's consume(LBRACE) ensures error messages point to the right
// source location if '{' is missing.
std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto node = std::make_unique<BlockStmt>();
    node->token = previous(); // The '{' token — used for error reporting

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->statements.push_back(parseStatement());
    }

    consume(TokenType::RBRACE, "Expected '}' to close block.");
    return node;
}

// if (condition) { } elif (condition) { } else { }
//
// 'if' has already been consumed by match() in parseStatement().
// The body is a block — we consume '{' here, then call parseBlock().
// Zero or more elif branches follow, then an optional else.
std::unique_ptr<IfStmt> Parser::parseIfStmt() {
    auto node = std::make_unique<IfStmt>();
    node->token = previous(); // 'if' token

    consume(TokenType::LPAREN, "Expected '(' after 'if'.");
    node->condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after if condition.");
    consume(TokenType::LBRACE, "Expected '{' before if body.");
    node->thenBranch = parseBlock();

    while (match({TokenType::ELIF})) {
        ElifBranch branch;
        consume(TokenType::LPAREN, "Expected '(' after 'elif'.");
        branch.condition = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after elif condition.");
        consume(TokenType::LBRACE, "Expected '{' before elif body.");
        branch.body = parseBlock();
        node->elifBranches.push_back(std::move(branch));
    }

    if (match({TokenType::ELSE})) {
        consume(TokenType::LBRACE, "Expected '{' before else body.");
        node->elseBranch = parseBlock();
    }

    return node;
}

// fore (mark16 i = 0; i < 10; i++) { }
//
// 'fore' has already been consumed. We parse the three-part header inside ( )
// then the body block.
std::unique_ptr<ForeStmt> Parser::parseForeStmt() {
    auto node = std::make_unique<ForeStmt>();
    node->token = previous(); // 'fore' token

    consume(TokenType::LPAREN, "Expected '(' after 'fore'.");

    // Initializer: mark16 i = 0;
    node->initType  = consumeType("Expected a valid type for fore loop variable (e.g., mark16).");
    node->initVar   = consume(TokenType::IDENT,   "Expected loop variable name.");
    consume(TokenType::ASSIGN, "Expected '=' in loop initializer.");
    node->initValue = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after loop initializer.");

    // Condition: i < 10;
    node->condition = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after loop condition.");

    // Increment: i++ — stored as an expression.
    // i++ is lexed as IDENT PLUS PLUS. parsePrecedence handles the ++ as
    // two PLUS tokens which won't form a valid expression by themselves.
    // TODO: For V1, the increment expression is best written as i = i + 1.
    // Full postfix ++ support would require adding a PostfixIncr expression node.
    node->increment = parseExpression();

    consume(TokenType::RPAREN, "Expected ')' after fore header.");
    consume(TokenType::LBRACE, "Expected '{' before fore body.");
    node->body = parseBlock();

    return node;
}

// whirl (condition) { }
std::unique_ptr<WhirlStmt> Parser::parseWhirlStmt() {
    auto node = std::make_unique<WhirlStmt>();
    node->token = previous(); // 'whirl' token

    consume(TokenType::LPAREN, "Expected '(' after 'whirl'.");
    node->condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after whirl condition.");
    consume(TokenType::LBRACE, "Expected '{' before whirl body.");
    node->body = parseBlock();

    return node;
}

// yield (ctrl, data);   yield 0;   yield;
//
// 'yield' has already been consumed. We check what follows to determine the form:
//   ';'  -> void return (abyss spells)
//   '('  -> tuple return
//   else -> single value return
//
// CodeGen note: Every yield in a spell that contains a destined block will be
// rewritten during CodeGen into: __ret = <values>; goto __destined_N;
// The YieldStmt node itself does not know about this rewrite — CodeGen handles it
// by scanning the parent spell for destined blocks during emission.
std::unique_ptr<YieldStmt> Parser::parseYieldStmt() {
    auto node = std::make_unique<YieldStmt>();
    node->token = previous(); // 'yield' token

    if (check(TokenType::SEMICOLON)) {
        advance(); // consume ';'
        return node; // Void return — values vector is empty
    }

    if (match({TokenType::LPAREN})) {
        // Tuple return: yield (ctrl, ruin(DiskError::HardwareFault));
        do {
            node->values.push_back(parseExpression());
        } while (match({TokenType::COMMA}));
        consume(TokenType::RPAREN, "Expected ')' after tuple yield values.");
    } else {
        // Single value: yield 0;
        node->values.push_back(parseExpression());
    }

    consume(TokenType::SEMICOLON, "Expected ';' after yield.");
    return node;
}

// destined (condition) { cleanup_body; }
// destined { cleanup_body; }            <- condition is OPTIONAL
//
// FIXED: The original parser always did consume(LPAREN) which would crash
// on the conditionless form. The spec explicitly states the condition is
// optional. We now check whether '(' or '{' follows and behave accordingly.
std::unique_ptr<DestinedStmt> Parser::parseDestinedStmt() {
    auto node = std::make_unique<DestinedStmt>();
    node->token = previous(); // 'destined' token

    // Optional condition: if '(' follows, parse condition; if '{', skip it.
    if (match({TokenType::LPAREN})) {
        node->hasCondition = true;
        node->condition    = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after destined condition.");
    } else {
        // No condition — cleanup fires unconditionally before every yield.
        node->hasCondition = false;
        // node->condition remains nullptr
    }

    consume(TokenType::LBRACE, "Expected '{' before destined body.");
    node->body = parseBlock();
    return node;
}

// my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
//     (ctrl, scroll data)                    => { ... }
//     (ctrl, ruin<DiskError::HardwareFault>) => { ... }
//     (ctrl, ruin err)                       => { ... }
// }
//
// FIXED: DivineBranch now uses distinct fields for each branch kind instead
// of reusing payloadVar for both the variant name and the error variable.
// This eliminates the ambiguity that would have caused silent semantic bugs.
std::unique_ptr<DivineStmt> Parser::parseDivineStmt() {
    auto node = std::make_unique<DivineStmt>();

    // IDENT <~ divine
    node->targetVar = consume(TokenType::IDENT,     "Expected variable name before '<~'.");
    consume(TokenType::REV_WEAVE,                   "Expected '<~' in divine statement.");
    node->token     = consume(TokenType::DIVINE,    "Expected 'divine' keyword.");

    // The spell call expression: fetch_sector(own &my_disk, 0x0500)
    node->spellCall = parseExpression();

    consume(TokenType::LBRACE, "Expected '{' before divine branches.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        DivineBranch branch;

        consume(TokenType::LPAREN, "Expected '(' before divine pattern.");

        // First element: the ownership rebinding variable (always present)
        branch.ownerVar = consume(TokenType::IDENT, "Expected ownership variable (e.g., 'ctrl').");

        // FIX — Ghost Variable trap:
        //   If ')' follows immediately, this is a payloadless success branch: (ctrl) => { }
        //   This is the correct syntax for 'abyss | ruin<T>' omens where the success
        //   carries no data. Parsing a payload token here would create a ghost variable —
        //   a name registered in the symbol table but never declared in the generated C.
        if (check(TokenType::RPAREN)) {
            branch.isRuin        = false;
            branch.isPayloadless = true;
            consume(TokenType::RPAREN, "Expected ')' after payloadless divine pattern.");
        } else {
            consume(TokenType::COMMA, "Expected ',' after ownership variable.");

            // Second element: the payload pattern
            if (match({TokenType::RUIN})) {
                branch.isRuin = true;

                if (match({TokenType::LT})) {
                    // Specific ruin: ruin<DiskError::HardwareFault>
                    branch.isSpecificRuin = true;
                    branch.rankName    = consume(TokenType::IDENT,  "Expected rank name in ruin<...>.");
                    consume(TokenType::SCOPE,                        "Expected '::' after rank name.");
                    branch.variantName = consume(TokenType::IDENT,  "Expected variant name after '::'.");
                    consume(TokenType::GT,                           "Expected '>' after ruin pattern.");
                } else {
                    // Catch-all ruin: ruin err
                    branch.isSpecificRuin = false;
                    branch.catchAllVar = consume(TokenType::IDENT, "Expected error variable name after 'ruin'.");
                }
            } else {
                // Success branch with payload: scroll data
                branch.isRuin        = false;
                branch.isPayloadless = false;
                branch.successType   = consumeType("Expected a valid type for success branch (e.g., scroll).");
                branch.successVar    = consume(TokenType::IDENT, "Expected success variable name.");
            }

            consume(TokenType::RPAREN, "Expected ')' after divine pattern.");
        }
        consume(TokenType::FAT_ARROW,  "Expected '=>' after divine pattern.");
        consume(TokenType::LBRACE, "Expected '{' before branch body.");
        branch.body = parseBlock();

        node->branches.push_back(std::move(branch));
    }

    consume(TokenType::RBRACE, "Expected '}' after divine branches.");
    return node;
}

// Handles both expression statements and assignment statements.
//
// We parse the left-hand side as an expression first. If '=' follows,
// it is an assignment statement. Otherwise it is a bare expression statement.
//
// Expression statements: process_data(data);    acknowledge_interrupt();
// Assignment statements: ctrl = Disk:Reading;   my_disk = Disk:Idle;
//
// This two-step approach correctly handles complex left-hand sides like
// ctrl->field = value; (a member access expression as the target).
std::unique_ptr<Stmt> Parser::parseExprOrAssignStmt() {
    auto expr = parseExpression();

    if (match({TokenType::ASSIGN})) {
        auto assign   = std::make_unique<AssignStmt>();
        assign->token  = previous(); // '=' token
        assign->target = std::move(expr);
        assign->value  = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after assignment.");
        return assign;
    }

    consume(TokenType::SEMICOLON, "Expected ';' after expression statement.");
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::unique_ptr<Stmt> Parser::parseVarDeclStmt(Token typeToken) {
    // We already ate the type token (e.g., 'mark16' or 'Device'). Now get the name.
    Token varName = consume(TokenType::IDENT, "Expected variable name.");

    std::unique_ptr<Expr> initializer = nullptr;
    if (match({TokenType::ASSIGN})) {
        initializer = parseExpression();
    }

    consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
    return std::make_unique<VarDeclStmt>(typeToken, varName, std::move(initializer));
}

// =============================================================================
// EXPRESSION PARSERS (Pratt Parsing / Precedence Climbing)
// =============================================================================

// Top-level entry. Starts at minimum precedence 1 so all infix operators
// are eligible from the beginning.
std::unique_ptr<Expr> Parser::parseExpression() {
    return parsePrecedence(1);
}

// The core of the Pratt Parser.
//
// HOW IT WORKS:
//   1. Consume the first token and create a PREFIX node (literal, identifier,
//      unary operator, parenthesized expression, etc.)
//   2. While the NEXT token is an infix operator whose precedence >= minPrecedence:
//      a. Consume the operator
//      b. Parse the right operand at (precedence + 1) to get left-associativity
//      c. Wrap left and right into a BinaryExpr (or CallExpr, PostfixExpr, etc.)
//      d. The result becomes the new 'left' for the next iteration
//
// WHY THIS WORKS:
//   By passing (prec + 1) for the right operand, we ensure that the NEXT
//   operator of the SAME precedence goes to a new iteration rather than being
//   consumed by the recursive call. This gives left-associativity: a + b + c
//   becomes (a + b) + c, not a + (b + c).
//
//   Higher-precedence operators consume more tightly: a + b * c correctly
//   becomes a + (b * c) because * has higher precedence than + so the recursive
//   call for b's right side will consume the *.
std::unique_ptr<Expr> Parser::parsePrecedence(int minPrecedence) {
    Token tok = advance(); // Consume the first token of this expression
    std::unique_ptr<Expr> left;

    // --- PREFIX RULES: What can START an expression? ---
    switch (tok.type) {

        // Integer and string literals: 0x1F7, 42, "Drive dead"
        case TokenType::INT_LIT:
        case TokenType::STRING_LIT:
            left = std::make_unique<LiteralExpr>(tok);
            break;

        // Boolean literals: kept (true), forsaken (false)
        case TokenType::KEPT:
        case TokenType::FORSAKEN:
            left = std::make_unique<LiteralExpr>(tok);
            break;

        // Identifier — could be:
        //   plain name:          my_disk
        //   stance reference:    Disk:Fault        (IDENT COLON IDENT)
        //   rank variant:        DiskError::Timeout (IDENT SCOPE IDENT)
        case TokenType::IDENT: {
            auto identExpr = std::make_unique<IdentifierExpr>(tok);

            if (match({TokenType::COLON})) {
                // Stance reference: Disk:Fault
                identExpr->stanceName = consume(TokenType::IDENT, "Expected stance name after ':'.");
            } else if (match({TokenType::SCOPE})) {
                // Rank variant: DiskError::Timeout
                identExpr->variantName = consume(TokenType::IDENT, "Expected variant name after '::'.");
            }

            left = std::move(identExpr);
            break;
        }

        // Parenthesized expression: (a + b)
        case TokenType::LPAREN:
            left = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after parenthesized expression.");
            break;

        // Address-of operator: &my_disk, &disk_data_port
        // Semantic analysis distinguishes hardware addresses from regular addresses.
        case TokenType::AMP:
            left = std::make_unique<AddressOfExpr>(tok, parsePrecedence(7));
            break;

        // Unary minus: -x
        // Pointer dereference: *ptr
        case TokenType::MINUS:
        case TokenType::STAR:
            left = std::make_unique<UnaryExpr>(tok, parsePrecedence(7));
            break;

        // ruin(DiskError::HardwareFault) — error value construction in yield.
        // We parse it as a special call-like expression because it looks like
        // a function call syntactically but has specific semantic meaning.
        case TokenType::RUIN: {
            auto ruinIdent = std::make_unique<IdentifierExpr>(tok);
            consume(TokenType::LPAREN, "Expected '(' after 'ruin'.");
            auto callNode = std::make_unique<CallExpr>(std::move(ruinIdent), tok);
            // The argument is the rank variant: DiskError::HardwareFault
            callNode->args.push_back(parsePrecedence(1));
            callNode->argIsOwned.push_back(false);
            consume(TokenType::RPAREN, "Expected ')' after ruin argument.");
            left = std::move(callNode);
            break;
        }

        default:
            error(tok, "Expected an expression.");
    }

    // --- INFIX RULES: What can FOLLOW an expression? ---
    while (true) {
        int prec = getPrecedence(peek().type);
        if (prec < minPrecedence) break; // Next operator is too low — stop climbing

        Token op = advance(); // Consume the infix operator

        if (op.type == TokenType::QUESTION) {
            // Postfix '?' — Omen unpack. No right operand needed.
            // read_buffer()? -> if ruin, yield it up; if success, unwrap.
            left = std::make_unique<PostfixExpr>(std::move(left), op);

        } else if (op.type == TokenType::LPAREN) {
            // Function call: callee(args...)
            // '(' was just consumed as the operator — parseCallExpr handles args.
            left = parseCallExpr(std::move(left));

        } else if (op.type == TokenType::ARROW || op.type == TokenType::DOT) {
            // Member access: ctrl->stance,  node.field
            // Right side is always a single identifier (not a full expression).
            Token member = consume(TokenType::IDENT, "Expected member name after '->' or '.'.");
            auto memberExpr = std::make_unique<IdentifierExpr>(member);
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(memberExpr));

        } else {
            // Standard binary operator: +, -, *, /, ==, !=, <, >, ~>, <~, |
            // Parse right side at (prec + 1) for left-associativity.
            auto right = parsePrecedence(prec + 1);
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
        }
    }

    return left;
}

// Parses the argument list of a function call.
// Called when '(' is detected after an expression in the infix loop above.
// The '(' token has already been consumed.
//
// Handles 'own' on arguments: fetch_sector(own &my_disk, 0x0500)
// The 'own' keyword is recorded in argIsOwned (compile-time tracking only).
// CodeGen strips 'own' and emits the bare argument expression.
std::unique_ptr<Expr> Parser::parseCallExpr(std::unique_ptr<Expr> callee) {
    Token parenToken = previous(); // The '(' token
    auto callNode = std::make_unique<CallExpr>(std::move(callee), parenToken);

    if (!check(TokenType::RPAREN)) {
        do {
            bool owned = match({TokenType::OWN}); // 'own' is optional per argument
            callNode->args.push_back(parseExpression());
            callNode->argIsOwned.push_back(owned);
        } while (match({TokenType::COMMA}));
    }

    consume(TokenType::RPAREN, "Expected ')' after function arguments.");
    return callNode;
}

// Maps each infix operator token type to its binding power (precedence level).
// Matches the Cgil spec v1.7 operator precedence table exactly.
//
// Higher number = binds tighter = evaluated first.
// 0 means "not an infix operator" — the climb stops when getPrecedence returns 0.
//
// Ref (from spec, highest to lowest):
//   ? (Omen Unpack)         8
//   () function call         8  (same level as ?, so a()? works left-to-right)
//   ->, .                    7
//   *, /                     6
//   +, -                     5
//   ==, !=, >, <             4
//   ~> (Weave)               3
//   <~ (Reverse Weave)       2
//   | (Omen Union)           1
int Parser::getPrecedence(TokenType type) const {
    switch (type) {
        case TokenType::QUESTION:  return 8; // ?  Omen unpack
        case TokenType::LPAREN:    return 8; // () Function call (same as ?)
        case TokenType::ARROW:
        case TokenType::DOT:       return 7; // ->, .  Member access
        case TokenType::STAR:
        case TokenType::SLASH:     return 6; // *,  /
        case TokenType::PLUS:
        case TokenType::MINUS:     return 5; // +,  -
        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::GT:
        case TokenType::LT:        return 4; // ==, !=, >, <
        case TokenType::WEAVE:     return 3; // ~>  Weave / pipeline
        case TokenType::REV_WEAVE: return 2; // <~  Reverse weave / extract
        case TokenType::PIPE:      return 1; // |   Omen union (lowest)
        default:                   return 0; // Not an infix operator
    }
}

// =============================================================================
// TOKEN NAVIGATION HELPERS
// =============================================================================

// Return the current token without consuming it.
Token Parser::peek() const {
    return tokens[current];
}

// Return the token one position ahead without consuming either token.
// Used in parseStatement() to detect the 'IDENT <~ divine' pattern.
//
// FIXED: Returns the last token in the stream (END_OF_FILE) if we are at or
// past the end, rather than accessing tokens[current + 1] unchecked. This
// prevents undefined behavior when peekNext() is called near the end of file.
Token Parser::peekNext() const {
    if (current + 1 >= (int)tokens.size()) return tokens.back();
    return tokens[current + 1];
}

// Return the most recently consumed token.
Token Parser::previous() const {
    return tokens[current - 1];
}

// True if the current token is END_OF_FILE.
bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

// Consume the current token and return it. Advances current by 1.
Token Parser::advance() {
    if (!isAtEnd()) current++;
    return previous();
}

// True if the current token matches 'type'. Does NOT consume the token.
bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type == type;
}

// If the current token matches any of the given types, consume it and return
// true. Otherwise return false without consuming anything.
bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

// Consume the current token if it is 'type'. If not, throw a descriptive
// error pointing at the current source location.
//
// This is called in every place where a specific token MUST appear. The error
// message is written to tell the programmer exactly what was expected and where.
Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(peek(), message);
}

// Consume the current token only if it is a valid Cgil type.
//
// WHY THIS EXISTS:
//   Several places in the parser need to consume a type token: parameter types,
//   field types, hardware declaration types, and for-loop init types. The naive
//   approach is advance() — just take whatever is next. The problem is that
//   advance() accepts any token, including integer literals, string literals, or
//   keywords that are not types. A typo like `spell foo(42 x)` would silently
//   store the literal 42 as the parameter type. Semantic analysis would then
//   try to look up "42" in the type system and crash with a confusing internal
//   error rather than a clean "expected a type" message at the source location.
//
//   consumeType() fixes this by explicitly checking membership in the set of
//   valid type tokens before consuming. If the check fails, the error is reported
//   immediately at the exact token that was wrong.
//
// ABYSS is included: it is valid as a return type in some positions.
// DECK and TUPLE are included: they appear in type positions in the spec.
// IDENT is included: sigil and legion type names are user-defined identifiers.
Token Parser::consumeType(const std::string& message) {
    if (match({TokenType::MARK16, TokenType::MARK32,
               TokenType::SOUL16, TokenType::SOUL32,
               TokenType::ADDR,   TokenType::FLOW,
               TokenType::RUNE,   TokenType::OATH,
               TokenType::SCROLL, TokenType::ABYSS,
               TokenType::DECK,   TokenType::TUPLE,
               TokenType::IDENT})) {
        return previous();
    }
    error(peek(), message);
}

// =============================================================================
// ERROR HANDLING
// =============================================================================

// Build a formatted error message with source location, then throw.
// [[noreturn]] tells the compiler this function never returns, which suppresses
// spurious "control reaches end of non-void function" warnings in callers.
[[noreturn]] void Parser::error(Token token, const std::string& message) {
    std::string err = "[Line " + std::to_string(token.line) +
                      ":" + std::to_string(token.column) + "] Parse Error";

    if (token.type == TokenType::END_OF_FILE) {
        err += " at end of file: " + message;
    } else {
        err += " at '" + token.lexeme + "': " + message;
    }

    throw std::runtime_error(err);
}

// After a parse error is caught by the top-level parse() loop, synchronize()
// skips tokens until it finds a point where parsing can safely resume.
//
// Safe boundaries (we stop at):
//   - A semicolon or closing brace (end of a statement or block)
//   - A keyword that starts a new top-level declaration
//
// This allows the parser to report multiple errors in one compiler run instead
// of stopping at the very first mistake. FIXED: Added RANK, LEGION, GRIMOIRE,
// PACT to the synchronize keyword set to match the full Cgil declaration set.
void Parser::synchronize() {
    advance(); // Skip the token that triggered the error

    while (!isAtEnd()) {
        if (previous().type == TokenType::SEMICOLON ||
            previous().type == TokenType::RBRACE) return;

        switch (peek().type) {
            case TokenType::SPELL:
            case TokenType::WARDEN:
            case TokenType::CONJURE:
            case TokenType::SIGIL:
            case TokenType::LEGION:
            case TokenType::LEYLINE:
            case TokenType::PORTLINE:
            case TokenType::RANK:
            case TokenType::GRIMOIRE:
            case TokenType::PACT:
                return;
            default:
                break;
        }

        advance();
    }
}
