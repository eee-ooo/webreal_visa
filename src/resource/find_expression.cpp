#include "resource/find_expression.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

namespace wrvisa {

struct FindNode {
    enum class Type { empty, literal, any, character_class, concatenation, alternate, repeat };

    Type type{Type::empty};
    char literal{0};
    bool negated{false};
    bool one_or_more{false};
    std::vector<std::pair<char, char>> ranges;
    std::vector<std::shared_ptr<const FindNode>> children;
};

namespace {

char fold(char value) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::shared_ptr<const FindNode> parse(std::string& error) {
        if (input_.empty() || input_.size() >= 256 || input_.find('{') != input_.npos ||
            input_.find('}') != input_.npos) {
            error = "empty, oversized, or attribute-filter expression";
            return {};
        }
        auto root = parse_alternate(error, 0);
        if (!root || position_ != input_.size()) {
            if (error.empty()) {
                error = "unexpected token";
            }
            return {};
        }
        return root;
    }

private:
    std::shared_ptr<const FindNode> make(FindNode node, std::string& error) {
        if (++nodes_ > 512) {
            error = "expression is too complex";
            return {};
        }
        return std::make_shared<FindNode>(std::move(node));
    }

    std::shared_ptr<const FindNode> parse_alternate(std::string& error,
                                                    std::size_t depth) {
        if (depth > 32) {
            error = "expression nesting is too deep";
            return {};
        }
        std::vector<std::shared_ptr<const FindNode>> branches;
        auto branch = parse_concatenation(error, depth);
        if (!branch) {
            return {};
        }
        branches.push_back(std::move(branch));
        while (position_ < input_.size() && input_[position_] == '|') {
            ++position_;
            auto next = parse_concatenation(error, depth);
            if (!next) {
                return {};
            }
            branches.push_back(std::move(next));
        }
        if (branches.size() == 1) {
            return branches.front();
        }
        FindNode node;
        node.type = FindNode::Type::alternate;
        node.children = std::move(branches);
        return make(std::move(node), error);
    }

    std::shared_ptr<const FindNode> parse_concatenation(std::string& error,
                                                        std::size_t depth) {
        std::vector<std::shared_ptr<const FindNode>> sequence;
        while (position_ < input_.size() && input_[position_] != ')' &&
               input_[position_] != '|') {
            auto atom = parse_atom(error, depth);
            if (!atom) {
                return {};
            }
            if (position_ < input_.size() &&
                (input_[position_] == '*' || input_[position_] == '+')) {
                FindNode repeat;
                repeat.type = FindNode::Type::repeat;
                repeat.one_or_more = input_[position_] == '+';
                repeat.children.push_back(std::move(atom));
                ++position_;
                atom = make(std::move(repeat), error);
                if (!atom) {
                    return {};
                }
                if (position_ < input_.size() &&
                    (input_[position_] == '*' || input_[position_] == '+')) {
                    error = "repeated quantifier";
                    return {};
                }
            }
            sequence.push_back(std::move(atom));
        }
        if (sequence.empty()) {
            error = "empty branch";
            return {};
        }
        if (sequence.size() == 1) {
            return sequence.front();
        }
        FindNode node;
        node.type = FindNode::Type::concatenation;
        node.children = std::move(sequence);
        return make(std::move(node), error);
    }

    std::shared_ptr<const FindNode> parse_atom(std::string& error, std::size_t depth) {
        if (position_ >= input_.size()) {
            error = "missing atom";
            return {};
        }
        const char token = input_[position_++];
        if (token == '(') {
            auto group = parse_alternate(error, depth + 1u);
            if (!group || position_ >= input_.size() || input_[position_] != ')') {
                if (error.empty()) {
                    error = "unclosed group";
                }
                return {};
            }
            ++position_;
            return group;
        }
        if (token == '[') {
            return parse_class(error);
        }
        if (token == '?' ) {
            FindNode node;
            node.type = FindNode::Type::any;
            return make(std::move(node), error);
        }
        if (token == '*' || token == '+' || token == ')') {
            error = "operator without atom";
            return {};
        }
        char literal = token;
        if (token == '\\') {
            if (position_ >= input_.size()) {
                error = "dangling escape";
                return {};
            }
            literal = input_[position_++];
        }
        FindNode node;
        node.type = FindNode::Type::literal;
        node.literal = fold(literal);
        return make(std::move(node), error);
    }

    char class_character(std::string& error) {
        if (position_ >= input_.size()) {
            error = "unclosed character class";
            return 0;
        }
        char value = input_[position_++];
        if (value == '\\') {
            if (position_ >= input_.size()) {
                error = "dangling class escape";
                return 0;
            }
            value = input_[position_++];
        }
        return fold(value);
    }

    std::shared_ptr<const FindNode> parse_class(std::string& error) {
        FindNode node;
        node.type = FindNode::Type::character_class;
        if (position_ < input_.size() && input_[position_] == '^') {
            node.negated = true;
            ++position_;
        }
        bool has_entry = false;
        while (position_ < input_.size() && input_[position_] != ']') {
            const char begin = class_character(error);
            if (!error.empty()) {
                return {};
            }
            char end = begin;
            if (position_ + 1u < input_.size() && input_[position_] == '-' &&
                input_[position_ + 1u] != ']') {
                ++position_;
                end = class_character(error);
                if (!error.empty() || begin > end) {
                    error = "invalid character range";
                    return {};
                }
            }
            node.ranges.emplace_back(begin, end);
            has_entry = true;
        }
        if (!has_entry || position_ >= input_.size() || input_[position_] != ']') {
            error = "empty or unclosed character class";
            return {};
        }
        ++position_;
        return make(std::move(node), error);
    }

    std::string_view input_;
    std::size_t position_{0};
    std::size_t nodes_{0};
};

using Positions = std::set<std::size_t>;

Positions match_node(const FindNode& node, std::string_view value, std::size_t position);

Positions repeat_closure(const FindNode& child, std::string_view value,
                         const Positions& initial) {
    Positions reached = initial;
    Positions frontier = initial;
    while (!frontier.empty()) {
        Positions next;
        for (const auto position : frontier) {
            for (const auto candidate : match_node(child, value, position)) {
                if (reached.insert(candidate).second) {
                    next.insert(candidate);
                }
            }
        }
        frontier = std::move(next);
    }
    return reached;
}

Positions match_node(const FindNode& node, std::string_view value, std::size_t position) {
    switch (node.type) {
        case FindNode::Type::empty:
            return {position};
        case FindNode::Type::literal:
            if (position < value.size() && fold(value[position]) == node.literal) {
                return {position + 1u};
            }
            return {};
        case FindNode::Type::any:
            return position < value.size() ? Positions{position + 1u} : Positions{};
        case FindNode::Type::character_class: {
            if (position >= value.size()) {
                return {};
            }
            const char character = fold(value[position]);
            const bool contained = std::any_of(
                node.ranges.begin(), node.ranges.end(), [character](const auto& range) {
                    return range.first <= character && character <= range.second;
                });
            return contained != node.negated ? Positions{position + 1u} : Positions{};
        }
        case FindNode::Type::concatenation: {
            Positions positions{position};
            for (const auto& child : node.children) {
                Positions next;
                for (const auto current : positions) {
                    const auto matched = match_node(*child, value, current);
                    next.insert(matched.begin(), matched.end());
                }
                positions = std::move(next);
                if (positions.empty()) {
                    break;
                }
            }
            return positions;
        }
        case FindNode::Type::alternate: {
            Positions positions;
            for (const auto& child : node.children) {
                const auto matched = match_node(*child, value, position);
                positions.insert(matched.begin(), matched.end());
            }
            return positions;
        }
        case FindNode::Type::repeat: {
            if (!node.one_or_more) {
                return repeat_closure(*node.children.front(), value, Positions{position});
            }
            const auto first = match_node(*node.children.front(), value, position);
            return repeat_closure(*node.children.front(), value, first);
        }
    }
    return {};
}

}  // namespace

std::optional<FindExpression> FindExpression::compile(std::string_view expression,
                                                       std::string& error) {
    error.clear();
    Parser parser(expression);
    auto root = parser.parse(error);
    if (!root) {
        return std::nullopt;
    }
    return FindExpression(std::move(root));
}

bool FindExpression::matches(std::string_view value) const {
    const auto positions = match_node(*root_, value, 0);
    return positions.contains(value.size());
}

}  // namespace wrvisa
