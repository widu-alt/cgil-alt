#include "Lexer.h"
#include "Parser.h"
#include "SemanticAnalyzer.h"
#include "CodeGen.h"
#include <iostream>
#include <fstream>
#include <sstream>

// Helper to read an entire file into a std::string
std::string readFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filepath);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: cgilc <source.gil>\n";
        return 1;
    }

    std::string sourcePath = argv[1];
    std::string outputPath = sourcePath.substr(0, sourcePath.find_last_of('.')) + ".c";

    try {
        std::cout << "[1/4] Reading " << sourcePath << "...\n";
        std::string sourceCode = readFile(sourcePath);

        std::cout << "[2/4] Lexing & Parsing...\n";
        Lexer lexer(sourceCode);
        std::vector<Token> tokens = lexer.tokenize();

        Parser parser(tokens);
        ProgramNode program;
        program.declarations = parser.parse();

        std::cout << "[3/4] Semantic Analysis (Pass 1 & 2)...\n";
        SemanticAnalyzer semantics;
        semantics.analyze(&program);

        std::cout << "[4/4] Code Generation...\n";
        std::ofstream outFile(outputPath);
        if (!outFile.is_open()) {
            throw std::runtime_error("Could not open output file: " + outputPath);
        }

        CodeGenVisitor codegen(outFile);
        codegen.generate(&program);

        std::cout << "Success! Compiled to -> " << outputPath << "\n";

    } catch (const std::exception& e) {
        // This catches your Lexer, Parser, and Semantic errors and prints
        // them cleanly with the Line/Column numbers you built.
        std::cerr << "\nCOMPILATION FAILED:\n" << e.what() << "\n";
        return 1;
    }

    return 0;
}