#include <print>

#include <slang/syntax/SyntaxTree.h>

int main() {
    auto tree = slang::syntax::SyntaxTree::fromText(R"(
        module top;
            logic a, b, y;
            assign y = a & b;
        endmodule
    )");

    std::println("parsed slang syntax tree, diagnostics: {}",
                 tree->diagnostics().empty() ? "none" : "present");
    return 0;
}
