#include "../../include/Lexer/Lexer.h"
#include "../../include/Parser/Parser.h"
#include "../../include/Semantics/SemanticAnalyzer.h"
#include "../../include/CodeGen/CodeGen.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio> // For std::remove

void printUsage() {
    std::cout << "Cgil Compiler Forge v1.0\n"
              << "Usage: cgilc <file.gil> [options]\n\n"
              << "Options:\n"
              << "  -o <file>        Specify the output executable name\n"
              << "  --emit-c         Stop after transpilation; do not invoke GCC, keep .c file\n"
              << "  --target=host    Compile as a standard desktop application (default)\n"
              << "  --target=kernel  Compile as bare-metal OS (applies ISR & hardware GCC flags)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string inputFile = "";
    std::string outputFile = "";
    bool emitC = false;
    bool targetKernel = false;

    // 1. Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-c") {
            emitC = true;
        } else if (arg == "--target=kernel") {
            targetKernel = true;
        } else if (arg == "--target=host") {
            targetKernel = false;
        } else if (arg == "-o") {
            if (i + 1 < argc) {
                outputFile = argv[++i];
            } else {
                std::cerr << "Error: -o requires a filename.\n";
                return 1;
            }
        } else if (arg[0] != '-') {
            inputFile = arg;
        } else {
            std::cerr << "Unknown flag: " << arg << "\n\n";
            printUsage();
            return 1;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "Error: No input .gil file specified.\n";
        return 1;
    }

    // Determine filenames
    std::string baseName = inputFile.substr(0, inputFile.find_last_of('.'));
    std::string cFilename = baseName + ".c";

    if (outputFile.empty()) {
        outputFile = baseName;
        if (!emitC && !targetKernel) {
            // Append .exe for Windows host builds if no explicit -o is given
            #ifdef _WIN32
            outputFile += ".exe";
            #endif
        } else if (!emitC && targetKernel) {
            // Bare metal usually outputs .o or .bin by default
            outputFile += ".o";
        }
    }

    // 2. Read Source File
    std::ifstream file(inputFile);
    if (!file.is_open()) {
        std::cerr << "Failed to open source file: " << inputFile << "\n";
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    try {
        std::cout << "[1/4] Reading " << inputFile << "...\n";
        
        std::cout << "[2/4] Lexing & Parsing...\n";
        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        
        // FIX 1: Wrap the raw vector of declarations into the root AST node
        ProgramNode program;
        program.declarations = parser.parse();

        std::cout << "[3/4] Semantic Analysis (Pass 1 & 2)...\n";
        SemanticAnalyzer sema;
        sema.analyze(&program);

        std::cout << "[4/4] Code Generation...\n";
        
        // FIX 2: Feed a stringstream directly into the CodeGen constructor
        std::ostringstream cCodeStream;
        CodeGenVisitor codegen(cCodeStream);
        codegen.generate(&program);
        std::string cCode = cCodeStream.str();

        // 3. Write intermediate C file

        // 3. Write intermediate C file
        std::ofstream outC(cFilename);
        if (!outC.is_open()) {
            std::cerr << "Failed to write intermediate file: " << cFilename << "\n";
            return 1;
        }
        outC << cCode;
        outC.close();

        // 4. Handle --emit-c (Stop here)
        if (emitC) {
            std::cout << "Success! Emitted C code to -> " << cFilename << "\n";
            return 0;
        }

        // 5. Invoke GCC
        std::string gccCmd = "gcc ";
        if (targetKernel) {
            // OS Kernel constraints: strictly GNU99, no SSE/Float registers, ignore integer casts for port I/O
            gccCmd += "-c -I. -mgeneral-regs-only -Wno-error=int-conversion -Wno-int-conversion -Wno-pointer-to-int-cast -std=gnu99 ";
        } else {
            // Host Windows/Linux constraints: standard warnings
            gccCmd += "-Wall -Wextra ";
        }
        
        gccCmd += cFilename + " -o " + outputFile;

        std::cout << "[GCC] " << gccCmd << "\n";
        int result = std::system(gccCmd.c_str());

        if (result == 0) {
            // 6. Cleanup intermediate file on success
            std::remove(cFilename.c_str());
            std::cout << "Success! Executable forged -> " << outputFile << "\n";
        } else {
            std::cerr << "\n[FATAL] GCC backend compilation failed. C code left intact at '" << cFilename << "' for debugging.\n";
            return result;
        }

    } catch (const std::exception& e) {
        std::cerr << "\nCOMPILATION FAILED:\n" << e.what() << "\n";
        return 1;
    }

    return 0;
}